#include "platform/platform.h"

#import <AppKit/AppKit.h>

#include <memory>

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

    bool pickDocuments(bool folder, std::function<void(std::vector<std::string>)> picked)
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
}
