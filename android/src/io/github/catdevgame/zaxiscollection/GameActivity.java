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

import org.libsdl.app.SDLActivity;

// SDLActivity runs the game (SDL_main in libps2EntryRunner.so, which has SDL3
// linked in statically) on the top screen; a device's second screen shows
// SecondScreenPanel, which comes and goes with the activity and the display.
// The setup's disc pickers are Documents'.
public class GameActivity extends SDLActivity implements DisplayManager.DisplayListener {
    private SecondScreenPanel mPanel;
    private boolean mStarted;

    @Override
    protected String[] getLibraries() {
        return new String[] { "ps2EntryRunner" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The game runs at 60: no second present of each frame at 120 Hz.
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredRefreshRate = 60.0f;
        getWindow().setAttributes(attributes);
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
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (!Documents.onActivityResult(this, requestCode, resultCode, data)) {
            super.onActivityResult(requestCode, resultCode, data);
        }
    }

    private void showPanel() {
        if (mPanel != null || mBrokenLibraries || !mStarted) {
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
