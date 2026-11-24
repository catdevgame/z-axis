#include "platform/platform.h"

#import <AppKit/AppKit.h>

#include <memory>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

extern char **environ;

namespace
{
    NSString *ns(const std::string &text)
    {
        return [NSString stringWithUTF8String:text.c_str()] ?: @"";
    }

    std::unique_ptr<zx_app::platform::MenuActions> g_actions;
    std::function<void()> g_openSettings;
    std::function<bool()> g_openSettingsAvailable;

    // Borderless full screen: the window's frame and style before.
    bool g_borderless = false;
    NSRect g_savedFrame = NSZeroRect;
    NSWindowStyleMask g_savedStyle = 0;

    NSWindow *gameWindow()
    {
        NSWindow *window = [NSApp mainWindow] ?: [NSApp keyWindow];
        if (!window && [[NSApp windows] count] > 0)
            window = [[NSApp windows] firstObject];
        return window;
    }
}

@interface ZXAppMenuTarget : NSObject <NSMenuItemValidation>
@end

@implementation ZXAppMenuTarget
- (void)toggleIntegerScaling:(id)sender
{
    (void)sender;
    if (g_actions && g_actions->toggleIntegerScaling)
        g_actions->toggleIntegerScaling();
}
- (void)toggleWidescreen:(id)sender
{
    (void)sender;
    if (g_actions && g_actions->toggleWidescreen)
        g_actions->toggleWidescreen();
}
- (void)togglePauseInBackground:(id)sender
{
    (void)sender;
    if (g_actions && g_actions->togglePauseInBackground)
        g_actions->togglePauseInBackground();
}
- (void)toggleMapWindow:(id)sender
{
    (void)sender;
    if (g_actions && g_actions->toggleMapWindow)
        g_actions->toggleMapWindow();
}
// Tag 0: CPU renderer; 1-4: GPU renderer at that internal resolution.
- (void)chooseRenderer:(id)sender
{
    const NSInteger tag = [(NSMenuItem *)sender tag];
    if (!g_actions || !g_actions->setRenderer)
        return;
    if (tag == 0)
        g_actions->setRenderer(false, g_actions->internalResolution ? g_actions->internalResolution() : 1);
    else
        g_actions->setRenderer(true, static_cast<int>(tag));
}
- (void)openSettings:(id)sender
{
    (void)sender;
    if (g_openSettings)
        g_openSettings();
}
- (void)showSettingsFolder:(id)sender
{
    (void)sender;
    if (g_actions)
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ns(g_actions->settingsFolder.string())]];
}
- (void)switchGame:(id)sender
{
    (void)sender;
    if (g_actions && g_actions->switchGame)
        g_actions->switchGame();
}
- (void)showLogsFolder:(id)sender
{
    (void)sender;
    if (g_actions)
        [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ns(g_actions->logsFolder.string())]];
}
- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (item.action == @selector(openSettings:))
        return g_openSettings && (!g_openSettingsAvailable || g_openSettingsAvailable());
    if (!g_actions)
        return NO;
    if (item.action == @selector(toggleIntegerScaling:) && g_actions->integerScaling)
        item.state = g_actions->integerScaling() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleWidescreen:) && g_actions->widescreen)
        item.state = g_actions->widescreen() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleWidescreen:) && g_actions->widescreenAvailable && !g_actions->widescreenAvailable())
        return NO;
    if (item.action == @selector(switchGame:) && g_actions->switchGameAvailable && !g_actions->switchGameAvailable())
        return NO;
    if (item.action == @selector(togglePauseInBackground:) && g_actions->pauseInBackground)
        item.state = g_actions->pauseInBackground() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleMapWindow:) && g_actions->mapWindow)
        item.state = g_actions->mapWindow() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(chooseRenderer:) && g_actions->gpuRenderer && g_actions->internalResolution)
    {
        const bool gpu = g_actions->gpuRenderer();
        const bool on = item.tag == 0 ? !gpu : (gpu && g_actions->internalResolution() == item.tag);
        item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    }
    return YES;
}
@end

// The open panel of pickFolderOrFile: folders, and files of its one type
// (a mod's .zip), can be picked; other files are shown greyed out.
@interface ZXFolderOrFilePanelFilter : NSObject <NSOpenSavePanelDelegate>
@property(nonatomic, copy) NSString *extension;
@end

@implementation ZXFolderOrFilePanelFilter
- (BOOL)panel:(id)sender shouldEnableURL:(NSURL *)url
{
    (void)sender;
    NSNumber *folder = nil;
    if ([url getResourceValue:&folder forKey:NSURLIsDirectoryKey error:nil] && folder.boolValue)
        return YES;
    return [url.pathExtension caseInsensitiveCompare:self.extension] == NSOrderedSame;
}
@end

namespace
{
    ZXAppMenuTarget *g_target = nil;

    NSMenuItem *addItem(NSMenu *menu, NSString *title, SEL action, NSString *key, NSEventModifierFlags modifiers, id target)
    {
        NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:key];
        item.keyEquivalentModifierMask = modifiers;
        item.target = target;
        return item;
    }
}

namespace zx_app::platform
{
    std::filesystem::path resourceFolder(const char *argv0)
    {
        @autoreleasepool
        {
            NSString *resources = [[NSBundle mainBundle] resourcePath];
            if (resources)
                return std::filesystem::path(resources.fileSystemRepresentation);
        }
        std::error_code ec;
        return std::filesystem::absolute(argv0 ? argv0 : "", ec).parent_path();
    }

    void showMessage(const std::string &title, const std::string &message, bool critical)
    {
        @autoreleasepool
        {
            [NSApp activateIgnoringOtherApps:YES];
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = ns(title);
            alert.informativeText = ns(message);
            alert.alertStyle = critical ? NSAlertStyleCritical : NSAlertStyleInformational;
            [alert addButtonWithTitle:@"OK"];
            [alert runModal];
        }
    }

    bool ask(const std::string &title, const std::string &message, const std::string &yes, const std::string &no)
    {
        @autoreleasepool
        {
            [NSApp activateIgnoringOtherApps:YES];
            NSAlert *alert = [[NSAlert alloc] init];
            alert.messageText = ns(title);
            alert.informativeText = ns(message);
            [alert addButtonWithTitle:ns(yes)];
            [alert addButtonWithTitle:ns(no)];
            return [alert runModal] == NSAlertFirstButtonReturn;
        }
    }

    void installMenus(const MenuActions &actions)
    {
        @autoreleasepool
        {
            g_actions = std::make_unique<MenuActions>(actions);
            g_target = [[ZXAppMenuTarget alloc] init];
            NSMenu *bar = [NSApp mainMenu];
            if (!bar)
            {
                bar = [[NSMenu alloc] init];
                [NSApp setMainMenu:bar];
            }

            NSMenu *file = [[NSMenu alloc] initWithTitle:@"File"];
            addItem(file, @"Show Settings Folder", @selector(showSettingsFolder:), @"", 0, g_target);
            addItem(file, @"Show Logs Folder", @selector(showLogsFolder:), @"", 0, g_target);
            if (g_actions && g_actions->switchGame)
            {
                [file addItem:[NSMenuItem separatorItem]];
                addItem(file, @"Switch Game…", @selector(switchGame:), @"", 0, g_target);
            }

            NSMenu *view = [[NSMenu alloc] initWithTitle:@"View"];
            // toggleFullScreen: goes to the key window; AppKit renames the
            // item to Exit Full Screen while in full screen.
            addItem(view, @"Enter Full Screen", @selector(toggleFullScreen:), @"f",
                    NSEventModifierFlagCommand | NSEventModifierFlagControl, nil);
            NSMenuItem *alternate = addItem(view, @"Full Screen", @selector(toggleFullScreen:), @"f",
                                            NSEventModifierFlagCommand, nil);
            alternate.alternate = YES;
            [view addItem:[NSMenuItem separatorItem]];
            addItem(view, @"Integer Scaling", @selector(toggleIntegerScaling:), @"", 0, g_target);
            if (g_actions->toggleWidescreen)
                addItem(view, @"Widescreen", @selector(toggleWidescreen:), @"", 0, g_target);
            [view addItem:[NSMenuItem separatorItem]];
            // The renderer: the CPU one, or the GPU one at 1x-4x.
            NSMenu *renderer = [[NSMenu alloc] initWithTitle:@"Renderer"];
            addItem(renderer, @"CPU (Accurate)", @selector(chooseRenderer:), @"", 0, g_target).tag = 0;
            [renderer addItem:[NSMenuItem separatorItem]];
            NSString *const gpuTitles[] = {@"GPU, Native (640x448)", @"GPU, 2x (1280x896)", @"GPU, 3x (1920x1344)",
                                           @"GPU, 4x (2560x1792)"};
            for (NSInteger scale = 1; scale <= 4; ++scale)
                addItem(renderer, gpuTitles[scale - 1], @selector(chooseRenderer:), @"", 0, g_target).tag = scale;
            NSMenuItem *rendererHolder = [view addItemWithTitle:@"Renderer" action:nil keyEquivalent:@""];
            rendererHolder.submenu = renderer;
            [view addItem:[NSMenuItem separatorItem]];
            addItem(view, @"Pause in Background", @selector(togglePauseInBackground:), @"", 0, g_target);
            if (g_actions->toggleMapWindow)
                addItem(view, @"Map Window", @selector(toggleMapWindow:), @"", 0, g_target);

            // Settings... (Cmd+,) in the app menu, after About.
            if (bar.numberOfItems > 0)
            {
                NSMenu *appMenu = [bar itemAtIndex:0].submenu;
                if (appMenu && [appMenu indexOfItemWithTitle:@"Settings…"] < 0)
                {
                    NSMenuItem *settings = [[NSMenuItem alloc] initWithTitle:@"Settings…"
                                                                      action:@selector(openSettings:)
                                                               keyEquivalent:@","];
                    settings.keyEquivalentModifierMask = NSEventModifierFlagCommand;
                    settings.target = g_target;
                    const NSInteger at = appMenu.numberOfItems > 0 ? 1 : 0;
                    [appMenu insertItem:[NSMenuItem separatorItem] atIndex:at];
                    [appMenu insertItem:settings atIndex:at + 1];
                }
            }

            // After the app menu, before Window.
            NSInteger index = bar.numberOfItems > 0 ? 1 : 0;
            for (NSMenu *menu in @[ file, view ])
            {
                NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:menu.title action:nil keyEquivalent:@""];
                holder.submenu = menu;
                [bar insertItem:holder atIndex:index++];
            }
        }
    }

    bool toggleFullscreen()
    {
        NSWindow *window = gameWindow();
        if (!window)
            return false;
        if (g_borderless)
            return setBorderless(false);
        [window toggleFullScreen:nil];
        return true;
    }

    bool isFullscreen(bool &known)
    {
        NSWindow *window = gameWindow();
        known = window != nil;
        return window && ((window.styleMask & NSWindowStyleMaskFullScreen) != 0 || g_borderless);
    }

    bool setBorderless(bool on)
    {
        NSWindow *window = gameWindow();
        if (!window)
            return false;
        if (on == g_borderless)
            return true;
        if (on)
        {
            if ((window.styleMask & NSWindowStyleMaskFullScreen) != 0)
                [window toggleFullScreen:nil];
            g_savedFrame = window.frame;
            g_savedStyle = window.styleMask;
            NSScreen *screen = window.screen ?: [NSScreen mainScreen];
            window.styleMask = NSWindowStyleMaskBorderless;
            [NSApp setPresentationOptions:NSApplicationPresentationAutoHideMenuBar |
                                          NSApplicationPresentationAutoHideDock];
            [window setFrame:screen.frame display:YES];
            [window makeKeyAndOrderFront:nil];
        }
        else
        {
            window.styleMask = g_savedStyle;
            [NSApp setPresentationOptions:NSApplicationPresentationDefault];
            [window setFrame:g_savedFrame display:YES];
            [window makeKeyAndOrderFront:nil];
        }
        g_borderless = on;
        return true;
    }

    bool isBorderless()
    {
        return g_borderless;
    }

    void setOpenSettingsAction(std::function<void()> action, std::function<bool()> available)
    {
        g_openSettings = std::move(action);
        g_openSettingsAvailable = std::move(available);
    }

    void frameInterpolationChanged(bool on)
    {
        (void)on;
    }

    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked)
    {
        (void)folder;
        (void)picked;
        return false;
    }

    bool pickDocumentsOnce(bool folder, std::function<void(std::vector<std::string>)> picked)
    {
        (void)folder;
        (void)picked;
        return false;
    }

    bool createDocument(const std::string &name, const std::string &mimeType, std::function<void(std::string)> created)
    {
        (void)name;
        (void)mimeType;
        (void)created;
        return false;
    }

    bool pickFolderOrFile(const std::string &extension, std::function<void(std::vector<std::string>)> picked)
    {
        @autoreleasepool
        {
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = YES;
            panel.allowsMultipleSelection = YES;
            panel.prompt = @"Add";
            ZXFolderOrFilePanelFilter *filter = [[ZXFolderOrFilePanelFilter alloc] init];
            filter.extension = ns(extension);
            panel.delegate = filter;
            auto shared = std::make_shared<std::function<void(std::vector<std::string>)>>(std::move(picked));
            // (the handler, copied by the panel, keeps the filter until the panel is done with it)
            void (^done)(NSModalResponse) = ^(NSModalResponse response) {
                panel.delegate = nil;
                (void)filter;
                std::vector<std::string> paths;
                if (response == NSModalResponseOK)
                    for (NSURL *url in panel.URLs)
                        if (url.fileURL && url.path)
                            paths.emplace_back(url.path.UTF8String);
                (*shared)(std::move(paths));
            };
            if (NSWindow *window = gameWindow())
                [panel beginSheetModalForWindow:window completionHandler:done];
            else
                [panel beginWithCompletionHandler:done];
        }
        return true;
    }

    // ---- the app's own update (collection/updater.h) ---------------------------

    namespace
    {
        // A system tool run to its end (its output, both streams, into
        // `output`); its exit status, -1 when it could not start.
        int runTool(const std::vector<std::string> &argv, std::string *output)
        {
            int pipeFds[2];
            if (pipe(pipeFds) != 0)
                return -1;
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
            posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
            std::vector<char *> args;
            for (const std::string &arg : argv)
                args.push_back(const_cast<char *>(arg.c_str()));
            args.push_back(nullptr);
            pid_t pid = -1;
            const int spawned = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            close(pipeFds[1]);
            char buffer[4096];
            for (ssize_t got; (got = read(pipeFds[0], buffer, sizeof(buffer))) > 0;)
                if (output && output->size() < 65536)
                    output->append(buffer, static_cast<size_t>(got));
            close(pipeFds[0]);
            if (spawned != 0)
                return -1;
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        // The tool's last line, for the menu's notice.
        std::string lastLine(std::string text)
        {
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            const size_t newline = text.rfind('\n');
            return newline == std::string::npos ? text : text.substr(newline + 1);
        }

        std::string bundleValue(const std::filesystem::path &app, NSString *key)
        {
            @autoreleasepool
            {
                NSString *plist = [NSString stringWithUTF8String:(app / "Contents" / "Info.plist").c_str()];
                NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:plist];
                id value = info[key];
                return [value isKindOfClass:[NSString class]] ? std::string([(NSString *)value UTF8String]) : std::string();
            }
        }
    }

    zx::collection::UpdatePlatform updatePlatform(const char *argv0)
    {
        (void)argv0;
        zx::collection::UpdatePlatform update;
        std::filesystem::path bundle;
        @autoreleasepool
        {
            NSString *path = [[NSBundle mainBundle] bundlePath];
            if (path)
                bundle = std::filesystem::path(path.fileSystemRepresentation);
        }
        // (a runner outside a bundle, a developer's build: nothing to update)
        if (bundle.extension() != ".app")
            return update;
        update.install = zx::collection::UpdatePlatform::Install::Bundle;
        update.packageSuffixes = {"-mac-arm64.zip", "-mac.zip"};
        update.installed = bundle;
        // (beside the bundle, on its volume: the new bundle is moved into its place, never copied)
        update.updateFolder = bundle.parent_path() / ("." + bundle.stem().string() + " update");
        update.download = [](const std::string &url, const std::filesystem::path &to, std::string *error)
        {
            // (curl, the system's own: redirects followed, as GitHub's release downloads need; an HTTP error fails it)
            std::string output;
            const int status = runTool({"/usr/bin/curl", "--fail", "--silent", "--show-error", "--location", "--retry", "2",
                                        "--connect-timeout", "20", "--output", to.string(), url},
                                       &output);
            if (status != 0 && error)
                *error = output.empty() ? "curl stopped (" + std::to_string(status) + ")" : lastLine(output);
            return status == 0;
        };
        update.unpack = [](const std::filesystem::path &zip, const std::filesystem::path &into, std::string *error)
        {
            // (ditto, as Finder unpacks: the bundle's symlinks, modes and seal kept)
            std::error_code ec;
            std::filesystem::create_directories(into, ec);
            std::string output;
            const int status = runTool({"/usr/bin/ditto", "-x", "-k", zip.string(), into.string()}, &output);
            if (status != 0 && error)
                *error = "ditto could not unpack " + zip.filename().string() + ": " + lastLine(output);
            return status == 0;
        };
        update.packageVersion = [](const std::filesystem::path &app) { return bundleValue(app, @"CFBundleShortVersionString"); };
        update.relaunch = [bundle]() -> long
        {
            // (the bundle now in place: its own executable, in this one's environment but the script that drove this run)
            const std::string executable = bundleValue(bundle, @"CFBundleExecutable");
            if (executable.empty())
                return -1;
            const std::string path = (bundle / "Contents" / "MacOS" / executable).string();
            unsetenv("ZX_MENU_SCRIPT");
            posix_spawnattr_t attributes;
            posix_spawnattr_init(&attributes);
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
            char *args[] = {const_cast<char *>(path.c_str()), nullptr};
            pid_t pid = -1;
            const int spawned = posix_spawn(&pid, path.c_str(), nullptr, &attributes, args, environ);
            posix_spawnattr_destroy(&attributes);
            return spawned == 0 ? static_cast<long>(pid) : -1;
        };
        return update;
    }
}
