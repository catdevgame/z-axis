package io.github.catdevgame.zaxiscollection;

import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.util.Arrays;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

// SDLActivity runs the game (SDL_main in libzx-runner.so, which has SDL3
// linked in statically) on the top screen; a device's second screen shows
// SecondScreenPanel, which comes and goes with the activity and the display.
// The setup's disc pickers are Documents'; the screenshots reach the
// system's Gallery through Gallery; the app's own update is Updater's.
public class GameActivity extends SDLActivity implements DisplayManager.DisplayListener {
    // The refresh rate asked of the top screen, as the game last asked for it
    // (askRefreshRate; collection/refresh_rate.h): the game's 60 until then.
    // Kept for an activity made again.
    private static volatile float sRefreshRate = 60.0f;

    private SecondScreenPanel mPanel;
    private boolean mStarted;

    @Override
    protected String[] getLibraries() {
        return new String[] { "zx-runner" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        applyRefreshRate();
        // The whole screen: without this the window's black status bar
        // background stays over the top of the picture.
        getWindow().setDecorFitsSystemWindows(false);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Color.TRANSPARENT);
        WindowInsetsController bars = getWindow().getInsetsController();
        if (bars != null) {
            bars.hide(WindowInsets.Type.systemBars());
            bars.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        if (!mBrokenLibraries) {
            Documents.install(this);
            Gallery.install(this);
            Updater.install(this);
        }
    }

    // From the game's thread (collection/platform_android.cpp), when Frame
    // Interpolation is turned on or off: the screen's refresh rates at its
    // present size, then the one to ask for.
    public float[] offeredRefreshRates() {
        Display display = getDisplay();
        if (display == null) {
            return new float[0];
        }
        Display.Mode current = display.getMode();
        Display.Mode[] modes = display.getSupportedModes();
        float[] rates = new float[modes.length];
        int count = 0;
        for (Display.Mode mode : modes) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                    && mode.getPhysicalHeight() == current.getPhysicalHeight()) {
                rates[count++] = mode.getRefreshRate();
            }
        }
        return Arrays.copyOf(rates, count);
    }

    public void askRefreshRate(float rate) {
        sRefreshRate = rate;
        runOnUiThread(this::applyRefreshRate);
    }

    private void applyRefreshRate() {
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredRefreshRate = sRefreshRate;
        getWindow().setAttributes(attributes);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (!Documents.onActivityResult(this, requestCode, resultCode, data)) {
            super.onActivityResult(requestCode, resultCode, data);
        }
    }

    private void showPanel() {
        if (mPanel != null || mBrokenLibraries || !mStarted || secondScreenOff()) {
            return;
        }
        DisplayManager displays = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        for (Display display : displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (display.getDisplayId() == Display.DEFAULT_DISPLAY) {
                continue;
            }
            try {
                SecondScreenPanel panel = new SecondScreenPanel(this, display);
                panel.show();
                mPanel = panel;
            } catch (WindowManager.InvalidDisplayException | WindowManager.BadTokenException e) {
                // (show() can fail while the display goes away)
            }
            return;
        }
    }

    // A developer switch: ZX_SECOND_SCREEN=off in the files folder's zx-env.txt (the native side's developer
    // switches) leaves a second display unused, so a dual-screen device runs as a single-screen one does (the
    // bottom screen then comes over the game by its shortcut, collection/panel_overlay.h).
    private boolean secondScreenOff() {
        File env = new File(getExternalFilesDir(null), "zx-env.txt");
        try (BufferedReader lines = new BufferedReader(new FileReader(env))) {
            for (String line = lines.readLine(); line != null; line = lines.readLine()) {
                if (line.trim().equals("ZX_SECOND_SCREEN=off")) {
                    return true;
                }
            }
        } catch (IOException e) {
            // (no such file: the switches' defaults)
        }
        return false;
    }

    private void hidePanel() {
        if (mPanel != null) {
            mPanel.dismiss();
            mPanel = null;
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        mStarted = true;
        showPanel();
        ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE))
                .registerDisplayListener(this, new Handler(Looper.getMainLooper()));
    }

    @Override
    protected void onStop() {
        mStarted = false;
        ((DisplayManager) getSystemService(Context.DISPLAY_SERVICE)).unregisterDisplayListener(this);
        hidePanel();
        super.onStop();
    }

    @Override
    public void onDisplayAdded(int displayId) {
        showPanel();
    }

    @Override
    public void onDisplayRemoved(int displayId) {
        if (mPanel != null && mPanel.getDisplay().getDisplayId() == displayId) {
            hidePanel();
        }
    }

    @Override
    public void onDisplayChanged(int displayId) {
    }
}
