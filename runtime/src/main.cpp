#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_host_backend.h"
#include "games_database.h"
#include "runtime/ps2_boot_sequence.h"
#include "runtime/ps2_front_end.h"
#if defined(PS2X_ENABLE_DEBUG_UI)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <vector>

#if defined(__ANDROID__)
#include <SDL3/SDL_main.h> // main becomes the SDL_main SDLActivity calls
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

#if defined(PS2X_PGO_GENERATE)
// Instrumented builds (clang -fprofile-instr-generate): write the profile
// before std::_Exit, which skips the runtime's own atexit writer.
extern "C" int __llvm_profile_write_file(void);
#endif

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    // Optional host path overrides, applied after loadELF() (which derives every
    // root from the ELF directory):
    //   PS2X_CD_IMAGE  raw 2048-byte/sector disc image (.iso) used for LBN reads
    //   PS2X_CD_ROOT   directory that backs cdrom0: file paths
    //   PS2X_HOST_ROOT directory that backs host0:
    //   PS2X_MC_ROOT   memory card directory
    //   PS2X_CD_FROM_IMAGE=1  cdrom0: files from the image, no CD root folder
    void applyIoPathEnvironmentOverrides()
    {
#if !defined(__ANDROID__)
        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        bool changed = false;
        auto apply = [&](const char *name, std::filesystem::path &target)
        {
            const char *value = std::getenv(name);
            if (value && value[0] != '\0')
            {
                target = std::filesystem::path(value);
                std::cout << "[io] " << name << "=" << value << std::endl;
                changed = true;
            }
        };
        apply("PS2X_CD_IMAGE", paths.cdImage);
        apply("PS2X_CD_ROOT", paths.cdRoot);
        apply("PS2X_HOST_ROOT", paths.hostRoot);
        apply("PS2X_MC_ROOT", paths.mcRoot);
        // PS2X_CD_FROM_IMAGE=1: cdrom0: files come from PS2X_CD_IMAGE (ISO9660)
        // rather than a CD root folder.
        if (const char *value = std::getenv("PS2X_CD_FROM_IMAGE"); value && value[0] == '1')
        {
            paths.cdFromImage = true;
            std::cout << "[io] PS2X_CD_FROM_IMAGE=1" << std::endl;
            changed = true;
        }
        if (changed)
        {
            PS2Runtime::setIoPaths(paths);
        }
#endif
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    // A front end built in (runtime/ps2_front_end.h): Android's preparation,
    // the packaged app.
    if (const ps2x::front_end::Prepare prepare = ps2x::front_end::prepare())
        prepare(argc, argv);
    setupTerminateLogger();

    // The packaged app: no ELF argument (a double-click) or a .iso.
    if (const ps2x::front_end::App &app = ps2x::front_end::app(); app.startedAsApp && app.startedAsApp(argc, argv))
    {
        try
        {
            return app.run(argc, argv);
        }
        catch (const std::exception &e)
        {
            std::cerr << "[app] fatal exception: " << e.what() << std::endl;
            return 1;
        }
    }

#if !defined(__ANDROID__)
    // --sequence ai:STEPFILE,dm2:STEPFILE: several games one after another
    // in this process (runtime/ps2_boot_sequence.h; tooling/checks/seqcheck.py).
    if (argc >= 3 && std::string(argv[1]) == "--sequence")
    {
        int status = 1;
        try
        {
            std::vector<ps2x::sequence::Step> steps;
            std::string error;
            if (!ps2x::sequence::parseArgument(argv[2], steps, &error))
            {
                std::cerr << "[seq] " << error << std::endl;
                status = 2;
            }
            else
            {
                const auto outcomes = ps2x::sequence::run(steps, "Z-Axis Collection | sequence",
                                                          [](PS2Runtime &) { applyIoPathEnvironmentOverrides(); });
                status = outcomes.size() == steps.size() && outcomes.back().booted ? 0 : 3;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "[seq] fatal exception: " << e.what() << std::endl;
        }
        std::cout.flush();
        std::cerr.flush();
#if defined(PS2X_PGO_GENERATE)
        __llvm_profile_write_file();
#endif
        std::_Exit(status);
    }
#endif

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "Z-Axis Collection | ";
        const char *gameName = getGameName(normalizedId);

        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
        {
            windowTitle += elfName;
        }

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI)
        // This hook is to prevent leak imgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        // PS2X_DEBUG_UI=0 starts with the debugger hidden (F1 still toggles it).
        if (const char *debugUi = std::getenv("PS2X_DEBUG_UI"); debugUi && debugUi[0] == '0')
        {
            debugPanel.setVisible(false);
        }
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        applyIoPathEnvironmentOverrides();

        runtime.run();
        ps2_game_overrides::runShutdownHooks(runtime);
        // The window, audio and gamepads (run() leaves the host up; the
        // destructor, which std::_Exit skips, would shut it down).
        ps2_host::shutdown();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
#if defined(PS2X_PGO_GENERATE)
        __llvm_profile_write_file();
#endif
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
