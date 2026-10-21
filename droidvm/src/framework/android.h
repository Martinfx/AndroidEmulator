// The emulated Android "system": activities, the view tree, input and drawing.
//
// On a real phone this is the job of the Java framework (android.jar), the
// system_server, SurfaceFlinger and the GPU. Here a single class plays all of
// those parts: it owns the activity stack and a framebuffer, runs the main
// loop's callbacks and turns mouse events into MotionEvents.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gfx/raster.h"
#include "vm/vm.h"

namespace droid {

struct Screen {
    int width = 720;       // pixels
    int height = 1280;
    float density = 2.0f;  // pixels per dp (2.0 = "xhdpi")
};

class AndroidRuntime {
public:
    AndroidRuntime(VM& vm, Screen screen, std::function<int64_t()> clockMs);
    ~AndroidRuntime();

    // Creates the activity and runs onCreate/onStart/onResume.
    void launch(const std::string& activityClassName, const std::string& appLabel);

    // Runs Handler callbacks that are due and expires toasts.
    void tick();
    // Draws the whole screen (status bar, action bar, content, toasts).
    void render();
    bool needsRedraw() const { return dirty_; }
    const Raster& framebuffer() const { return raster_; }

    // Input, in screen pixels. action: 0 down, 1 up, 2 move.
    void touch(int action, float x, float y);
    void scroll(float x, float y, float dy);  // mouse wheel, dy > 0 scrolls content up
    void textInput(const std::u16string& text);
    void backspace();
    void back();  // the Android back button

    bool finished() const { return activities_.empty(); }
    int exitCode() const { return exitCode_; }
    void shutdown();
    int64_t now() const { return clock_(); }
    // Milliseconds until the next scheduled callback (or -1 when none).
    int64_t msUntilNextTimer() const;

    // --- used by the native framework classes --------------------------------
    VM& vm() { return vm_; }
    const Screen& screen() const { return screen_; }
    Raster& raster() { return raster_; }
    void invalidate() { dirty_ = true; }
    void requestLayout() { dirty_ = true; layoutDirty_ = true; }
    void post(Object* runnable, int64_t delayMs);
    void removeCallbacks(Object* runnable);
    void showToast(const std::u16string& text, int64_t durationMs);
    void startActivity(Object* intent);
    void finishActivity(Object* activity);
    Object* focusedView() const { return focused_; }
    void setFocus(Object* view);
    std::u16string& titleOf(Object* activity);

    static AndroidRuntime* current;

private:
    struct Timer {
        int64_t when;
        uint64_t seq;
        Object* runnable;
    };
    struct Toast {
        std::u16string text;
        int64_t until;
    };

    void createActivity(Class* c, Object* intent);
    void processFinished();
    // Runs a callback into Java; an uncaught exception "crashes" the app like on Android.
    bool guarded(const std::function<void()>& fn);
    void layoutIfNeeded();
    int contentTop() const;

    VM& vm_;
    Screen screen_;
    std::function<int64_t()> clock_;
    Raster raster_;
    std::vector<Object*> activities_;
    std::vector<Timer> timers_;
    std::vector<Toast> toasts_;
    uint64_t timerSeq_ = 0;
    Object* canvas_ = nullptr;
    Object* focused_ = nullptr;
    Object* touchTarget_ = nullptr;
    Object* scrollCandidate_ = nullptr;
    float downX_ = 0, downY_ = 0;
    float lastScrollY_ = 0;
    bool scrolling_ = false;
    bool dirty_ = true;
    bool layoutDirty_ = true;
    int exitCode_ = 0;
    std::string appLabel_;
};

}  // namespace droid
