// Native implementation of a small part of the Android framework:
// Activity, the View system (View, ViewGroup, LinearLayout, FrameLayout,
// ScrollView, TextView, Button, EditText), Canvas/Paint/Color, Handler,
// Toast, Log, Intent, Bundle and a few helpers.
//
// The important trick: measuring, laying out, drawing and touch handling are
// done by calling the *virtual* Java methods onMeasure/onLayout/onDraw/
// onTouchEvent. Our built-in widgets implement those methods natively, so an
// app class that extends View and overrides onDraw() just works.
#include "framework/android.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <iostream>
#include <map>
#include <tuple>

#include "framework/natives.h"

namespace droid {

AndroidRuntime* AndroidRuntime::current = nullptr;

namespace {

// ------------------------------------------------------------------ peers

enum class Widget { View, TextView, Button, EditText };

struct ViewPeer : NativePeer {
    Object* context = nullptr;
    Object* parent = nullptr;
    std::vector<Object*> children;
    Object* clickListener = nullptr;
    Object* touchListener = nullptr;
    Object* layoutParams = nullptr;
    Object* tag = nullptr;

    Widget widget = Widget::View;
    int id = -1;
    int visibility = 0;  // VISIBLE 0, INVISIBLE 4, GONE 8
    bool enabled = true;
    bool clickable = false;
    bool pressed = false;
    bool hasBackground = false;
    uint32_t background = 0;
    int padL = 0, padT = 0, padR = 0, padB = 0;
    int minW = 0, minH = 0;
    int left = 0, top = 0, right = 0, bottom = 0;  // relative to the parent
    int measuredW = 0, measuredH = 0;
    bool measuredSet = false;

    // TextView and friends
    std::u16string text, hint;
    float textSize = 28;  // px
    uint32_t textColor = 0xDE000000;
    int gravity = 0x33;   // TOP | LEFT
    bool allCaps = false;

    // ViewGroups
    bool isGroup = false;
    int orientation = 0;  // LinearLayout: 0 horizontal, 1 vertical
    bool isScroll = false;
    int scrollY = 0;

    int width() const { return right - left; }
    int height() const { return bottom - top; }

    void trace(std::vector<Object*>& out) const override {
        out.insert(out.end(), children.begin(), children.end());
        for (Object* o : {context, parent, clickListener, touchListener, layoutParams, tag}) out.push_back(o);
    }
};

struct ActivityPeer : NativePeer {
    Object* contentView = nullptr;
    Object* intent = nullptr;
    std::u16string title;
    bool finishing = false;
    void trace(std::vector<Object*>& out) const override { out.push_back(contentView); out.push_back(intent); }
};

struct PaintPeer : NativePeer {
    uint32_t color = 0xff000000;
    int style = 0;  // FILL, STROKE, FILL_AND_STROKE
    float strokeWidth = 0;
    float textSize = 12;
    int align = 0;  // LEFT, CENTER, RIGHT
};

struct MotionEventPeer : NativePeer {
    int action = 0;
    float x = 0, y = 0, rawX = 0, rawY = 0;
    int64_t time = 0;
};

struct ExtrasPeer : NativePeer {  // Intent extras and Bundle contents
    Class* target = nullptr;
    std::map<std::u16string, Object*> objects;
    std::map<std::u16string, Reg> values;
    void trace(std::vector<Object*>& out) const override {
        for (auto& [k, v] : objects) out.push_back(v);
    }
};

struct ToastPeer : NativePeer {
    std::u16string text;
    int duration = 0;
};

ViewPeer* view(Object* o) { return o ? o->peerAs<ViewPeer>() : nullptr; }
ViewPeer* view(Reg r) { return view(asObj(r)); }
AndroidRuntime& rt() { return *AndroidRuntime::current; }
float dp(float v) { return v * rt().screen().density; }

// Names of fields in our native LayoutParams classes.
struct LayoutParamsFields {
    Field *width, *height, *leftMargin, *topMargin, *rightMargin, *bottomMargin, *weight, *gravity, *frameGravity;
} lpf;

struct LP {
    int width = -2, height = -2;  // MATCH_PARENT -1, WRAP_CONTENT -2
    int ml = 0, mt = 0, mr = 0, mb = 0;
    float weight = 0;
    int gravity = -1;
};

LP layoutParams(Object* child, int defaultWidth, int defaultHeight) {
    LP lp;
    lp.width = defaultWidth;
    lp.height = defaultHeight;
    Object* o = view(child)->layoutParams;
    if (!o) return lp;
    lp.width = asInt(o->fields[lpf.width->slot]);
    lp.height = asInt(o->fields[lpf.height->slot]);
    Class* margin = rt().vm().findClass("Landroid/view/ViewGroup$MarginLayoutParams;");
    if (o->klass->isSubclassOf(margin)) {
        lp.ml = asInt(o->fields[lpf.leftMargin->slot]);
        lp.mt = asInt(o->fields[lpf.topMargin->slot]);
        lp.mr = asInt(o->fields[lpf.rightMargin->slot]);
        lp.mb = asInt(o->fields[lpf.bottomMargin->slot]);
    }
    if (o->klass->isSubclassOf(rt().vm().findClass("Landroid/widget/LinearLayout$LayoutParams;"))) {
        lp.weight = asFloat(o->fields[lpf.weight->slot]);
        lp.gravity = asInt(o->fields[lpf.gravity->slot]);
    }
    if (o->klass->isSubclassOf(rt().vm().findClass("Landroid/widget/FrameLayout$LayoutParams;")))
        lp.gravity = asInt(o->fields[lpf.frameGravity->slot]);
    return lp;
}

// ------------------------------------------------------------ MeasureSpec
//
// A measure spec packs a mode and a size into one int, exactly like Android:
// the top two bits are the mode.
constexpr int UNSPECIFIED = 0, EXACTLY = 1 << 30, AT_MOST = 2 << 30;
int specMode(int spec) { return int(uint32_t(spec) & 0xC0000000u); }
int specSize(int spec) { return int(uint32_t(spec) & 0x3FFFFFFFu); }
int makeSpec(int size, int mode) { return int((uint32_t(std::max(size, 0)) & 0x3FFFFFFFu) | uint32_t(mode)); }

// Same rules as ViewGroup.getChildMeasureSpec().
int childSpec(int parentSpec, int padding, int childDim) {
    int mode = specMode(parentSpec);
    int size = std::max(0, specSize(parentSpec) - padding);
    if (childDim >= 0) return makeSpec(childDim, EXACTLY);
    if (mode == EXACTLY) return makeSpec(size, childDim == -1 ? EXACTLY : AT_MOST);
    if (mode == AT_MOST) return makeSpec(size, AT_MOST);
    return makeSpec(0, UNSPECIFIED);
}

int resolveSize(int desired, int spec) {
    switch (specMode(spec)) {
    case EXACTLY: return specSize(spec);
    case AT_MOST: return std::min(desired, specSize(spec));
    default: return desired;
    }
}

void measure(Object* v, int ws, int hs) {
    rt().vm().callVirtual(v, "onMeasure", "(II)V", {fromInt(ws), fromInt(hs)});
}

void layout(Object* v, int l, int t, int r, int b) {
    rt().vm().callVirtual(v, "layout", "(IIII)V", {fromInt(l), fromInt(t), fromInt(r), fromInt(b)});
}

bool isGone(Object* v) { return view(v)->visibility == 8; }

// ------------------------------------------------------------- text layout

std::u16string displayText(const ViewPeer* p) {
    std::u16string t = p->text;
    if (p->allCaps)
        for (auto& c : t) c = javaToUpper(c);
    return t;
}

// Greedy word wrapping into lines that fit maxWidth (if > 0).
std::vector<std::u16string> wrapText(const std::u16string& text, float size, float maxWidth) {
    std::vector<std::u16string> lines;
    std::u16string line;
    size_t i = 0;
    auto flush = [&] { lines.push_back(line); line.clear(); };
    while (i <= text.size()) {
        if (i == text.size() || text[i] == '\n') {
            flush();
            ++i;
            if (i > text.size()) break;
            continue;
        }
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
        std::u16string word = text.substr(i, j - i);
        std::u16string candidate = line.empty() ? word : line + u" " + word;
        if (maxWidth <= 0 || Raster::textWidth(candidate, size) <= maxWidth || line.empty()) {
            line = candidate;
            // A single word wider than the line: break it by characters.
            while (maxWidth > 0 && Raster::textWidth(line, size) > maxWidth && line.size() > 1) {
                size_t k = line.size();
                while (k > 1 && Raster::textWidth(line.substr(0, k), size) > maxWidth) --k;
                lines.push_back(line.substr(0, k));
                line = line.substr(k);
            }
        } else {
            flush();
            line = word;
        }
        i = j;
        if (i < text.size() && text[i] == ' ') ++i;
    }
    if (lines.empty()) lines.push_back(u"");
    return lines;
}

void setMeasured(Object* v, int w, int h) {
    ViewPeer* p = view(v);
    p->measuredW = w;
    p->measuredH = h;
    p->measuredSet = true;
}

// ----------------------------------------------------- LinearLayout logic

void measureLinear(Object* self, int ws, int hs) {
    ViewPeer* p = view(self);
    bool vertical = p->orientation == 1;
    // "main" is the stacking axis, "cross" the other one.
    int mainSpec = vertical ? hs : ws, crossSpec = vertical ? ws : hs;
    int mainPad = vertical ? p->padT + p->padB : p->padL + p->padR;
    int crossPad = vertical ? p->padL + p->padR : p->padT + p->padB;
    int used = 0, maxCross = 0;
    float totalWeight = 0;
    std::vector<Object*> weighted;

    auto measureChild = [&](Object* c, const LP& lp, int mainUsed, int forcedMain) {
        int mainMargin = vertical ? lp.mt + lp.mb : lp.ml + lp.mr;
        int crossMargin = vertical ? lp.ml + lp.mr : lp.mt + lp.mb;
        int crossDim = vertical ? lp.width : lp.height;
        int mainDim = vertical ? lp.height : lp.width;
        int cs = childSpec(crossSpec, crossPad + crossMargin, crossDim);
        int msp = forcedMain >= 0 ? makeSpec(forcedMain, EXACTLY) : childSpec(mainSpec, mainPad + mainMargin + mainUsed, mainDim);
        if (vertical) measure(c, cs, msp); else measure(c, msp, cs);
    };

    for (Object* c : p->children) {
        if (isGone(c)) continue;
        LP lp = layoutParams(c, vertical ? -1 : -2, -2);
        if (lp.weight > 0 && specMode(mainSpec) == EXACTLY) {
            totalWeight += lp.weight;
            weighted.push_back(c);
            used += vertical ? lp.mt + lp.mb : lp.ml + lp.mr;
            continue;
        }
        measureChild(c, lp, used, -1);
        ViewPeer* cp = view(c);
        used += (vertical ? cp->measuredH + lp.mt + lp.mb : cp->measuredW + lp.ml + lp.mr);
        maxCross = std::max(maxCross, (vertical ? cp->measuredW + lp.ml + lp.mr : cp->measuredH + lp.mt + lp.mb));
    }
    // Share the remaining space between children with layout_weight.
    if (!weighted.empty()) {
        int remaining = std::max(0, specSize(mainSpec) - mainPad - used);
        for (Object* c : weighted) {
            LP lp = layoutParams(c, vertical ? -1 : -2, -2);
            int share = int(remaining * lp.weight / totalWeight);
            remaining -= share;
            totalWeight -= lp.weight;
            measureChild(c, lp, 0, share);
            ViewPeer* cp = view(c);
            used += vertical ? cp->measuredH : cp->measuredW;
            maxCross = std::max(maxCross, (vertical ? cp->measuredW + lp.ml + lp.mr : cp->measuredH + lp.mt + lp.mb));
        }
    }
    int mainSize = resolveSize(std::max(used + mainPad, vertical ? p->minH : p->minW), mainSpec);
    int crossSize = resolveSize(std::max(maxCross + crossPad, vertical ? p->minW : p->minH), crossSpec);
    setMeasured(self, vertical ? crossSize : mainSize, vertical ? mainSize : crossSize);

    // MATCH_PARENT children on the cross axis get the final size (Android's forceUniformWidth).
    if (specMode(crossSpec) != EXACTLY) {
        int exact = makeSpec(crossSize, EXACTLY);
        for (Object* c : p->children) {
            if (isGone(c)) continue;
            LP lp = layoutParams(c, vertical ? -1 : -2, -2);
            if ((vertical ? lp.width : lp.height) != -1) continue;
            ViewPeer* cp = view(c);
            int keepMain = makeSpec(vertical ? cp->measuredH : cp->measuredW, EXACTLY);
            int cs = childSpec(exact, crossPad + (vertical ? lp.ml + lp.mr : lp.mt + lp.mb), -1);
            if (vertical) measure(c, cs, keepMain); else measure(c, keepMain, cs);
        }
    }
}

// Horizontal / vertical position of a box of `size` inside [start, end) per gravity bits.
int gravityOffset(int gravity, bool horizontal, int space) {
    int g = horizontal ? (gravity & 0x07) : (gravity & 0x70);
    if (horizontal) {
        if ((gravity & 0x00800000) && (gravity & 0x07) == 0x05) g = 0x05;  // END
        if (g == 0x01) return space / 2;  // CENTER_HORIZONTAL
        if (g == 0x05) return space;      // RIGHT
        return 0;
    }
    if (g == 0x10) return space / 2;  // CENTER_VERTICAL
    if (g == 0x50) return space;      // BOTTOM
    return 0;
}

void layoutLinear(Object* self) {
    ViewPeer* p = view(self);
    bool vertical = p->orientation == 1;
    int contentW = p->width() - p->padL - p->padR, contentH = p->height() - p->padT - p->padB;
    int total = 0;
    for (Object* c : p->children) {
        if (isGone(c)) continue;
        LP lp = layoutParams(c, vertical ? -1 : -2, -2);
        total += vertical ? view(c)->measuredH + lp.mt + lp.mb : view(c)->measuredW + lp.ml + lp.mr;
    }
    int pos = vertical ? p->padT + gravityOffset(p->gravity, false, std::max(0, contentH - total))
                       : p->padL + gravityOffset(p->gravity, true, std::max(0, contentW - total));
    for (Object* c : p->children) {
        if (isGone(c)) continue;
        LP lp = layoutParams(c, vertical ? -1 : -2, -2);
        ViewPeer* cp = view(c);
        int g = lp.gravity >= 0 ? lp.gravity : p->gravity;
        if (vertical) {
            pos += lp.mt;
            int x = p->padL + lp.ml + gravityOffset(g, true, std::max(0, contentW - cp->measuredW - lp.ml - lp.mr));
            layout(c, x, pos, x + cp->measuredW, pos + cp->measuredH);
            pos += cp->measuredH + lp.mb;
        } else {
            pos += lp.ml;
            int y = p->padT + lp.mt + gravityOffset(g, false, std::max(0, contentH - cp->measuredH - lp.mt - lp.mb));
            layout(c, pos, y, pos + cp->measuredW, y + cp->measuredH);
            pos += cp->measuredW + lp.mr;
        }
    }
}

// ------------------------------------------------------ FrameLayout logic

void measureFrame(Object* self, int ws, int hs) {
    ViewPeer* p = view(self);
    int maxW = 0, maxH = 0;
    for (Object* c : p->children) {
        if (isGone(c)) continue;
        LP lp = layoutParams(c, -1, -1);
        int cw = childSpec(ws, p->padL + p->padR + lp.ml + lp.mr, lp.width);
        // A ScrollView lets its child be as tall as it wants.
        int ch = p->isScroll ? makeSpec(0, UNSPECIFIED) : childSpec(hs, p->padT + p->padB + lp.mt + lp.mb, lp.height);
        measure(c, cw, ch);
        maxW = std::max(maxW, view(c)->measuredW + lp.ml + lp.mr);
        maxH = std::max(maxH, view(c)->measuredH + lp.mt + lp.mb);
    }
    setMeasured(self, resolveSize(std::max(maxW + p->padL + p->padR, p->minW), ws),
                resolveSize(std::max(maxH + p->padT + p->padB, p->minH), hs));
}

void layoutFrame(Object* self) {
    ViewPeer* p = view(self);
    int contentW = p->width() - p->padL - p->padR, contentH = p->height() - p->padT - p->padB;
    for (Object* c : p->children) {
        if (isGone(c)) continue;
        LP lp = layoutParams(c, -1, -1);
        ViewPeer* cp = view(c);
        int g = lp.gravity >= 0 ? lp.gravity : 0x33;
        int x = p->padL + lp.ml + gravityOffset(g, true, std::max(0, contentW - cp->measuredW - lp.ml - lp.mr));
        int y = p->padT + lp.mt + (p->isScroll ? 0 : gravityOffset(g, false, std::max(0, contentH - cp->measuredH - lp.mt - lp.mb)));
        layout(c, x, y, x + cp->measuredW, y + cp->measuredH);
    }
    if (p->isScroll && !p->children.empty()) {
        int maxScroll = std::max(0, view(p->children[0])->measuredH - contentH);
        p->scrollY = std::clamp(p->scrollY, 0, maxScroll);
    }
}

// --------------------------------------------------------------- drawing

void drawView(VM& vm, Object* v, Object* canvas) {
    ViewPeer* p = view(v);
    if (p->visibility != 0) return;
    Raster& r = rt().raster();
    r.save();
    r.translate(float(p->left), float(p->top));
    r.clipRect(0, 0, float(p->width()), float(p->height()));

    if (p->widget == Widget::Button && !p->hasBackground) {
        // A flat Material-style button background.
        uint32_t c = !p->enabled ? 0xffe0e0e0 : p->pressed ? 0xffbdbdbd : 0xffd6d7d7;
        r.fillRoundRect(dp(4), dp(6), p->width() - dp(4), p->height() - dp(6), dp(2), c);
    } else if (p->hasBackground) {
        r.fillRect(0, 0, float(p->width()), float(p->height()), p->background);
        if (p->pressed) r.fillRect(0, 0, float(p->width()), float(p->height()), 0x33000000);
    }
    vm.callVirtual(v, "onDraw", "(Landroid/graphics/Canvas;)V", {fromObj(canvas)});
    if (p->isGroup) {
        r.translate(0, float(-p->scrollY));
        for (Object* c : std::vector<Object*>(p->children)) drawView(vm, c, canvas);
    }
    r.restore();
}

void drawTextView(Object* v) {
    ViewPeer* p = view(v);
    Raster& r = rt().raster();
    float contentW = float(p->width() - p->padL - p->padR);
    float contentH = float(p->height() - p->padT - p->padB);
    bool showHint = p->text.empty() && !p->hint.empty();
    std::u16string text = showHint ? p->hint : displayText(p);
    uint32_t color = showHint ? 0x61000000 : p->enabled ? p->textColor : 0x61000000;
    auto lines = wrapText(text, p->textSize, contentW);
    float lh = Raster::lineHeight(p->textSize);
    float blockH = lh * float(lines.size());
    float y = float(p->padT) + float(gravityOffset(p->gravity, false, int(std::max(0.f, contentH - blockH))));
    for (auto& line : lines) {
        float w = Raster::textWidth(line, p->textSize);
        float x = float(p->padL) + float(gravityOffset(p->gravity, true, int(std::max(0.f, contentW - w))));
        r.text(line, x, y + Raster::ascent(p->textSize), p->textSize, color);
        y += lh;
    }
    if (p->widget == Widget::EditText) {
        bool focused = rt().focusedView() == v;
        float lineY = float(p->height()) - dp(8);
        r.fillRect(dp(4), lineY, float(p->width()) - dp(4), lineY + dp(focused ? 2 : 1), focused ? 0xffff4081 : 0x8a000000);
        if (focused && (rt().now() / 500) % 2 == 0) {
            const std::u16string& last = lines.back();
            float cx = float(p->padL) + (showHint ? 0 : Raster::textWidth(last, p->textSize));
            float cy = float(p->padT) + lh * float(lines.size() - 1);
            r.fillRect(cx, cy + dp(2), cx + dp(1), cy + lh - dp(2), 0xffff4081);
        }
    }
}

// ----------------------------------------------------------- touch helpers

// Absolute position of a view's top-left corner in content coordinates.
void absolutePosition(Object* v, float& x, float& y) {
    x = y = 0;
    for (Object* o = v; o; o = view(o)->parent) {
        x += float(view(o)->left);
        y += float(view(o)->top);
        if (Object* parent = view(o)->parent) y -= float(view(parent)->scrollY);
    }
}

void hitPath(Object* v, float x, float y, std::vector<Object*>& path) {
    ViewPeer* p = view(v);
    if (p->visibility != 0 || x < p->left || y < p->top || x >= p->right || y >= p->bottom) return;
    path.push_back(v);
    float lx = x - float(p->left), ly = y - float(p->top) + float(p->scrollY);
    for (auto it = p->children.rbegin(); it != p->children.rend(); ++it) {
        size_t before = path.size();
        hitPath(*it, lx, ly, path);
        if (path.size() != before) return;
    }
}

Object* makeMotionEvent(VM& vm, int action, float x, float y, float rawX, float rawY) {
    Object* ev = vm.alloc(vm.findClass("Landroid/view/MotionEvent;"));
    auto* mp = ev->peerAs<MotionEventPeer>();
    mp->action = action;
    mp->x = x;
    mp->y = y;
    mp->rawX = rawX;
    mp->rawY = rawY;
    mp->time = rt().now();
    return ev;
}

// Delivers an event to one view: OnTouchListener first, then onTouchEvent().
bool deliverTouch(VM& vm, Object* v, int action, float contentX, float contentY, float rawX, float rawY) {
    float ax, ay;
    absolutePosition(v, ax, ay);
    Object* ev = makeMotionEvent(vm, action, contentX - ax, contentY - ay, rawX, rawY);
    if (Object* l = view(v)->touchListener) {
        if (asInt(vm.callVirtual(l, "onTouch", "(Landroid/view/View;Landroid/view/MotionEvent;)Z", {fromObj(v), fromObj(ev)})))
            return true;
    }
    return asInt(vm.callVirtual(v, "onTouchEvent", "(Landroid/view/MotionEvent;)Z", {fromObj(ev)})) != 0;
}

}  // namespace

// ================================================================ runtime

AndroidRuntime::AndroidRuntime(VM& vm, Screen screen, std::function<int64_t()> clockMs)
    : vm_(vm), screen_(screen), clock_(std::move(clockMs)), raster_(screen.width, screen.height) {
    current = this;
    canvas_ = vm_.alloc(vm_.findClass("Landroid/graphics/Canvas;"));
    vm_.addRoot(canvas_);
}

AndroidRuntime::~AndroidRuntime() {
    if (current == this) current = nullptr;
}

int AndroidRuntime::contentTop() const { return int(80 * screen_.density); }  // status bar 24dp + action bar 56dp

bool AndroidRuntime::guarded(const std::function<void()>& fn) {
    try {
        fn();
        return true;
    } catch (JavaThrow& t) {
        std::cerr << "FATAL EXCEPTION: main\n" << vm_.describeException(t.exception) << "\n";
        if (Method* m = t.exception->klass->findVirtual("printStackTrace", "()V")) {
            Reg self = fromObj(t.exception);
            try { vm_.invoke(m, &self); } catch (...) {}
        }
        for (Object* a : activities_) vm_.removeRoot(a);
        activities_.clear();
        exitCode_ = 1;
    } catch (ExitRequest& e) {
        for (Object* a : activities_) vm_.removeRoot(a);
        activities_.clear();
        exitCode_ = e.code;
    }
    return false;
}

void AndroidRuntime::launch(const std::string& activityClassName, const std::string& appLabel) {
    appLabel_ = appLabel;
    Class* c = vm_.findClassOrNull(javaNameToDescriptor(activityClassName));
    if (!c) throw std::runtime_error("activity class not found: " + activityClassName);
    createActivity(c, nullptr);
}

void AndroidRuntime::createActivity(Class* c, Object* intent) {
    guarded([&] {
        if (!activities_.empty()) {
            Object* prev = activities_.back();
            vm_.callVirtual(prev, "onPause", "()V");
            vm_.callVirtual(prev, "onStop", "()V");
        }
        Object* activity = vm_.construct(c);
        vm_.addRoot(activity);
        auto* ap = activity->peerAs<ActivityPeer>();
        if (!ap) throw std::runtime_error(c->javaName() + " is not an Activity");
        ap->intent = intent;
        ap->title = utf8ToUtf16(appLabel_);
        activities_.push_back(activity);
        focused_ = nullptr;
        vm_.callVirtual(activity, "onCreate", "(Landroid/os/Bundle;)V", {0});
        vm_.callVirtual(activity, "onStart", "()V");
        vm_.callVirtual(activity, "onResume", "()V");
        requestLayout();
    });
    processFinished();
}

void AndroidRuntime::processFinished() {
    while (!activities_.empty() && activities_.back()->peerAs<ActivityPeer>()->finishing) {
        Object* a = activities_.back();
        guarded([&] {
            vm_.callVirtual(a, "onPause", "()V");
            vm_.callVirtual(a, "onStop", "()V");
            vm_.callVirtual(a, "onDestroy", "()V");
        });
        if (activities_.empty()) return;  // crashed
        activities_.pop_back();
        vm_.removeRoot(a);
        focused_ = touchTarget_ = scrollCandidate_ = nullptr;
        if (!activities_.empty()) {
            Object* top = activities_.back();
            guarded([&] {
                vm_.callVirtual(top, "onRestart", "()V");
                vm_.callVirtual(top, "onStart", "()V");
                vm_.callVirtual(top, "onResume", "()V");
            });
        }
        requestLayout();
    }
}

void AndroidRuntime::startActivity(Object* intent) {
    auto* ep = intent ? intent->peerAs<ExtrasPeer>() : nullptr;
    if (!ep || !ep->target) {
        vm_.warnOnce("startActivity: only explicit intents (new Intent(context, Foo.class)) are supported");
        return;
    }
    createActivity(ep->target, intent);
}

void AndroidRuntime::finishActivity(Object* activity) {
    activity->peerAs<ActivityPeer>()->finishing = true;
}

std::u16string& AndroidRuntime::titleOf(Object* activity) { return activity->peerAs<ActivityPeer>()->title; }

void AndroidRuntime::post(Object* runnable, int64_t delayMs) {
    if (!runnable) return;
    vm_.addRoot(runnable);
    timers_.push_back({now() + std::max<int64_t>(0, delayMs), timerSeq_++, runnable});
}

void AndroidRuntime::removeCallbacks(Object* runnable) {
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->runnable == runnable) { vm_.removeRoot(runnable); it = timers_.erase(it); }
        else ++it;
    }
}

int64_t AndroidRuntime::msUntilNextTimer() const {
    int64_t best = -1;
    for (auto& t : timers_) {
        int64_t d = std::max<int64_t>(0, t.when - now());
        if (best < 0 || d < best) best = d;
    }
    for (auto& t : toasts_) {
        int64_t d = std::max<int64_t>(0, t.until - now());
        if (best < 0 || d < best) best = d;
    }
    return best;
}

void AndroidRuntime::tick() {
    // Run due callbacks in time order; callbacks may post new ones.
    for (int guard = 0; guard < 1000 && !finished(); ++guard) {
        auto it = std::min_element(timers_.begin(), timers_.end(), [](const Timer& a, const Timer& b) {
            return a.when != b.when ? a.when < b.when : a.seq < b.seq;
        });
        if (it == timers_.end() || it->when > now()) break;
        Object* r = it->runnable;
        timers_.erase(it);
        guarded([&] { vm_.callVirtual(r, "run", "()V"); });
        vm_.removeRoot(r);
        processFinished();
    }
    size_t before = toasts_.size();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return t.until <= now(); }),
                  toasts_.end());
    if (toasts_.size() != before) invalidate();
    if (focused_) invalidate();  // blinking cursor
    if (vm_.allocatedSinceGc() > 20000 && vm_.frames.empty()) vm_.collectGarbage();
}

void AndroidRuntime::showToast(const std::u16string& text, int64_t durationMs) {
    toasts_.push_back({text, now() + durationMs});
    invalidate();
}

void AndroidRuntime::setFocus(Object* v) {
    focused_ = v;
    invalidate();
}

void AndroidRuntime::layoutIfNeeded() {
    if (!layoutDirty_ || finished()) return;
    layoutDirty_ = false;
    Object* root = activities_.back()->peerAs<ActivityPeer>()->contentView;
    if (!root) return;
    int w = screen_.width, h = screen_.height - contentTop();
    guarded([&] {
        measure(root, makeSpec(w, EXACTLY), makeSpec(h, EXACTLY));
        layout(root, 0, 0, w, h);
    });
}

void AndroidRuntime::render() {
    dirty_ = false;
    layoutIfNeeded();
    Raster& r = raster_;
    float d = screen_.density;
    r.fill(0xfffafafa);

    // Status bar with a clock, then the action bar with the activity title.
    r.fillRect(0, 0, float(screen_.width), 24 * d, 0xff303f9f);
    std::time_t t = std::time(nullptr);
    char clock[8];
    std::strftime(clock, sizeof clock, "%H:%M", std::localtime(&t));
    std::u16string clockText = utf8ToUtf16(clock);
    r.text(clockText, screen_.width - 16 * d - Raster::textWidth(clockText, 14 * d), 17 * d, 14 * d, 0xffffffff);
    r.fillRect(0, 24 * d, float(screen_.width), 80 * d, 0xff3f51b5);
    r.fillRect(0, 80 * d, float(screen_.width), 82 * d, 0x22000000);  // shadow
    if (!finished()) r.text(titleOf(activities_.back()), 16 * d, 59 * d, 20 * d, 0xffffffff);

    if (!finished()) {
        if (Object* root = activities_.back()->peerAs<ActivityPeer>()->contentView) {
            r.save();
            r.translate(0, float(contentTop()));
            r.clipRect(0, 0, float(screen_.width), float(screen_.height - contentTop()));
            guarded([&] { drawView(vm_, root, canvas_); });
            r.restore();
        }
    }

    // Toasts: a dark pill near the bottom of the screen.
    if (!toasts_.empty()) {
        const std::u16string& text = toasts_.back().text;
        float size = 14 * d;
        float tw = Raster::textWidth(text, size);
        float w = tw + 32 * d, h = 48 * d;
        float x = (screen_.width - w) / 2, y = screen_.height - 64 * d - h;
        r.fillRoundRect(x, y, x + w, y + h, 24 * d, 0xe0323232);
        r.text(text, x + 16 * d, y + h / 2 + Raster::ascent(size) / 2 - 2 * d, size, 0xffffffff);
    }
}

void AndroidRuntime::touch(int action, float x, float y) {
    if (finished()) return;
    Object* root = activities_.back()->peerAs<ActivityPeer>()->contentView;
    float cy = y - float(contentTop());
    if (!root) return;

    if (action == 0) {
        touchTarget_ = scrollCandidate_ = nullptr;
        scrolling_ = false;
        downX_ = x;
        downY_ = y;
        lastScrollY_ = y;
        std::vector<Object*> path;
        hitPath(root, x, cy, path);
        for (Object* v : path)
            if (view(v)->isScroll) scrollCandidate_ = v;
        guarded([&] {
            // The deepest view gets the first chance to handle the touch.
            for (auto it = path.rbegin(); it != path.rend(); ++it) {
                if (deliverTouch(vm_, *it, 0, x, cy, x, y)) { touchTarget_ = *it; break; }
            }
        });
    } else if (action == 2 && scrollCandidate_ && !scrolling_ && std::fabs(y - downY_) > 8 * screen_.density) {
        // Dragging vertically inside a ScrollView: it takes over the gesture.
        if (touchTarget_ && touchTarget_ != scrollCandidate_)
            guarded([&] { deliverTouch(vm_, touchTarget_, 3, x, cy, x, y); });  // ACTION_CANCEL
        touchTarget_ = nullptr;
        scrolling_ = true;
    }
    if (scrolling_ && action == 2) {
        ViewPeer* sp = view(scrollCandidate_);
        sp->scrollY += int(lastScrollY_ - y);
        layoutFrame(scrollCandidate_);  // clamps scrollY
        lastScrollY_ = y;
        invalidate();
    } else if (action != 0 && touchTarget_) {
        Object* target = touchTarget_;
        guarded([&] { deliverTouch(vm_, target, action, x, cy, x, y); });
    }
    if (action == 1) { touchTarget_ = nullptr; scrolling_ = false; }
    processFinished();
}

void AndroidRuntime::scroll(float x, float y, float dy) {
    if (finished()) return;
    Object* root = activities_.back()->peerAs<ActivityPeer>()->contentView;
    if (!root) return;
    std::vector<Object*> path;
    hitPath(root, x, y - float(contentTop()), path);
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        if (view(*it)->isScroll) {
            view(*it)->scrollY += int(dy);
            layoutFrame(*it);
            invalidate();
            return;
        }
    }
}

void AndroidRuntime::textInput(const std::u16string& text) {
    if (!focused_) return;
    view(focused_)->text += text;
    requestLayout();
}

void AndroidRuntime::backspace() {
    if (!focused_) return;
    auto& t = view(focused_)->text;
    if (!t.empty()) t.pop_back();
    requestLayout();
}

void AndroidRuntime::back() {
    if (finished()) return;
    if (focused_) { setFocus(nullptr); return; }
    Object* top = activities_.back();
    guarded([&] { vm_.callVirtual(top, "onBackPressed", "()V"); });
    processFinished();
}

void AndroidRuntime::shutdown() {
    while (!activities_.empty()) {
        activities_.back()->peerAs<ActivityPeer>()->finishing = true;
        processFinished();
    }
}

// ============================================================ the classes

namespace {

void defineInterface(VM& vm, const std::string& desc, std::vector<std::string> supers = {}) {
    vm.defineNativeClass(desc, "Ljava/lang/Object;", std::move(supers), kPublic | kInterface | kAbstract);
}

Class* defineEnum(VM& vm, const std::string& desc) {
    return vm.defineNativeClass(desc, "Ljava/lang/Enum;", {}, kPublic | kFinal);
}

int enumOrdinal(Object* e) {
    if (!e) return 0;
    return asInt(e->fields[e->klass->findField("ordinal")->slot]);
}

void registerViews(VM& vm) {
    defineInterface(vm, "Landroid/view/ViewParent;");
    defineInterface(vm, "Landroid/view/View$OnClickListener;");
    defineInterface(vm, "Landroid/view/View$OnTouchListener;");
    defineInterface(vm, "Landroid/text/Editable;", {"Ljava/lang/CharSequence;"});
    // EditText.getText() hands out a plain String, so let String pass as Editable.
    vm.stringClass->interfaces.push_back(vm.findClass("Landroid/text/Editable;"));

    // --- View ---------------------------------------------------------------
    Class* v = vm.defineNativeClass("Landroid/view/View;", "Ljava/lang/Object;");
    v->peerFactory = [] { return std::make_unique<ViewPeer>(); };
    auto init = NATIVE { view(a[0])->context = asObj(a[1]); return 0; };
    v->addNative("<init>", "(Landroid/content/Context;)V", init);
    v->addNative("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V", init);
    v->addNative("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;I)V", init);
    v->addNative("getContext", "()Landroid/content/Context;", NATIVE { return fromObj(view(a[0])->context); });
    v->addNative("setBackgroundColor", "(I)V", NATIVE {
        view(a[0])->hasBackground = true;
        view(a[0])->background = uint32_t(asInt(a[1]));
        rt().invalidate();
        return 0;
    });
    v->addNative("setPadding", "(IIII)V", NATIVE {
        ViewPeer* p = view(a[0]);
        p->padL = asInt(a[1]); p->padT = asInt(a[2]); p->padR = asInt(a[3]); p->padB = asInt(a[4]);
        rt().requestLayout();
        return 0;
    });
    v->addNative("getPaddingLeft", "()I", NATIVE { return fromInt(view(a[0])->padL); });
    v->addNative("getPaddingTop", "()I", NATIVE { return fromInt(view(a[0])->padT); });
    v->addNative("getPaddingRight", "()I", NATIVE { return fromInt(view(a[0])->padR); });
    v->addNative("getPaddingBottom", "()I", NATIVE { return fromInt(view(a[0])->padB); });
    v->addNative("setMinimumWidth", "(I)V", NATIVE { view(a[0])->minW = asInt(a[1]); rt().requestLayout(); return 0; });
    v->addNative("setMinimumHeight", "(I)V", NATIVE { view(a[0])->minH = asInt(a[1]); rt().requestLayout(); return 0; });
    v->addNative("setOnClickListener", "(Landroid/view/View$OnClickListener;)V", NATIVE {
        view(a[0])->clickListener = asObj(a[1]);
        view(a[0])->clickable = true;
        return 0;
    });
    v->addNative("setOnTouchListener", "(Landroid/view/View$OnTouchListener;)V", NATIVE { view(a[0])->touchListener = asObj(a[1]); return 0; });
    v->addNative("setClickable", "(Z)V", NATIVE { view(a[0])->clickable = asInt(a[1]); return 0; });
    v->addNative("isClickable", "()Z", NATIVE { return view(a[0])->clickable; });
    v->addNative("setEnabled", "(Z)V", NATIVE { view(a[0])->enabled = asInt(a[1]); rt().invalidate(); return 0; });
    v->addNative("isEnabled", "()Z", NATIVE { return view(a[0])->enabled; });
    v->addNative("isPressed", "()Z", NATIVE { return view(a[0])->pressed; });
    v->addNative("performClick", "()Z", NATIVE {
        Object* l = view(a[0])->clickListener;
        if (!l) return 0;
        vm.callVirtual(l, "onClick", "(Landroid/view/View;)V", {a[0]});
        return 1;
    });
    v->addNative("invalidate", "()V", NATIVE { rt().invalidate(); return 0; });
    v->addNative("postInvalidate", "()V", NATIVE { rt().invalidate(); return 0; });
    v->addNative("requestLayout", "()V", NATIVE { rt().requestLayout(); return 0; });
    v->addNative("getWidth", "()I", NATIVE { return fromInt(view(a[0])->width()); });
    v->addNative("getHeight", "()I", NATIVE { return fromInt(view(a[0])->height()); });
    v->addNative("getLeft", "()I", NATIVE { return fromInt(view(a[0])->left); });
    v->addNative("getTop", "()I", NATIVE { return fromInt(view(a[0])->top); });
    v->addNative("getRight", "()I", NATIVE { return fromInt(view(a[0])->right); });
    v->addNative("getBottom", "()I", NATIVE { return fromInt(view(a[0])->bottom); });
    v->addNative("getMeasuredWidth", "()I", NATIVE { return fromInt(view(a[0])->measuredW); });
    v->addNative("getMeasuredHeight", "()I", NATIVE { return fromInt(view(a[0])->measuredH); });
    v->addNative("setMeasuredDimension", "(II)V", NATIVE { setMeasured(asObj(a[0]), asInt(a[1]), asInt(a[2])); return 0; });
    v->addNative("measure", "(II)V", NATIVE { measure(asObj(a[0]), asInt(a[1]), asInt(a[2])); return 0; });
    v->addNative("onMeasure", "(II)V", NATIVE {
        // View.getDefaultSize(): take whatever the parent offers.
        ViewPeer* p = view(a[0]);
        auto def = [](int minimum, int spec) { return specMode(spec) == UNSPECIFIED ? minimum : specSize(spec); };
        setMeasured(asObj(a[0]), def(p->minW, asInt(a[1])), def(p->minH, asInt(a[2])));
        return 0;
    });
    v->addNative("layout", "(IIII)V", NATIVE {
        ViewPeer* p = view(a[0]);
        int ow = p->width(), oh = p->height();
        p->left = asInt(a[1]); p->top = asInt(a[2]); p->right = asInt(a[3]); p->bottom = asInt(a[4]);
        bool changed = ow != p->width() || oh != p->height();
        if (changed)
            vm.callVirtual(asObj(a[0]), "onSizeChanged", "(IIII)V", {fromInt(p->width()), fromInt(p->height()), fromInt(ow), fromInt(oh)});
        vm.callVirtual(asObj(a[0]), "onLayout", "(ZIIII)V", {fromInt(changed), a[1], a[2], a[3], a[4]});
        return 0;
    });
    v->addNative("onLayout", "(ZIIII)V", NATIVE { return 0; });
    v->addNative("onSizeChanged", "(IIII)V", NATIVE { return 0; });
    v->addNative("onDraw", "(Landroid/graphics/Canvas;)V", NATIVE { return 0; });
    v->addNative("onTouchEvent", "(Landroid/view/MotionEvent;)Z", NATIVE {
        // The default behaviour of a clickable view: pressed state + click on release.
        ViewPeer* p = view(a[0]);
        if (!p->clickable && p->widget != Widget::EditText) return 0;
        if (!p->enabled) return 1;
        auto* ev = asObj(a[1])->peerAs<MotionEventPeer>();
        bool inside = ev->x >= 0 && ev->y >= 0 && ev->x < p->width() && ev->y < p->height();
        switch (ev->action) {
        case 0: p->pressed = true; break;
        case 2: p->pressed = inside; break;
        case 3: p->pressed = false; break;
        case 1:
            if (p->pressed) {
                p->pressed = false;
                if (p->widget == Widget::EditText) rt().setFocus(asObj(a[0]));
                vm.callVirtual(asObj(a[0]), "performClick", "()Z");
            }
            break;
        }
        rt().invalidate();
        return 1;
    });
    v->addNative("setId", "(I)V", NATIVE { view(a[0])->id = asInt(a[1]); return 0; });
    v->addNative("getId", "()I", NATIVE { return fromInt(view(a[0])->id); });
    v->addNative("findViewById", "(I)Landroid/view/View;", NATIVE {
        std::function<Object*(Object*)> find = [&](Object* o) -> Object* {
            if (view(o)->id == asInt(a[1])) return o;
            for (Object* c : view(o)->children)
                if (Object* r = find(c)) return r;
            return nullptr;
        };
        return fromObj(find(asObj(a[0])));
    });
    v->addNative("setVisibility", "(I)V", NATIVE { view(a[0])->visibility = asInt(a[1]); rt().requestLayout(); return 0; });
    v->addNative("getVisibility", "()I", NATIVE { return fromInt(view(a[0])->visibility); });
    v->addNative("setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V", NATIVE {
        view(a[0])->layoutParams = asObj(a[1]);
        rt().requestLayout();
        return 0;
    });
    v->addNative("getLayoutParams", "()Landroid/view/ViewGroup$LayoutParams;", NATIVE { return fromObj(view(a[0])->layoutParams); });
    v->addNative("getParent", "()Landroid/view/ViewParent;", NATIVE { return fromObj(view(a[0])->parent); });
    v->addNative("setTag", "(Ljava/lang/Object;)V", NATIVE { view(a[0])->tag = asObj(a[1]); return 0; });
    v->addNative("getTag", "()Ljava/lang/Object;", NATIVE { return fromObj(view(a[0])->tag); });
    v->addNative("post", "(Ljava/lang/Runnable;)Z", NATIVE { rt().post(asObj(a[1]), 0); return 1; });
    v->addNative("postDelayed", "(Ljava/lang/Runnable;J)Z", NATIVE { rt().post(asObj(a[1]), asLong(a[2])); return 1; });
    v->addNative("removeCallbacks", "(Ljava/lang/Runnable;)Z", NATIVE { rt().removeCallbacks(asObj(a[1])); return 1; });
    v->addNative("getResources", "()Landroid/content/res/Resources;", NATIVE {
        Object* c = view(a[0])->context;
        return c ? vm.callVirtual(c, "getResources", "()Landroid/content/res/Resources;") : 0;
    });

    Class* ms = vm.defineNativeClass("Landroid/view/View$MeasureSpec;", "Ljava/lang/Object;");
    ms->addStaticNative("makeMeasureSpec", "(II)I", NATIVE { return fromInt(makeSpec(asInt(a[0]), asInt(a[1]))); });
    ms->addStaticNative("getMode", "(I)I", NATIVE { return fromInt(specMode(asInt(a[0]))); });
    ms->addStaticNative("getSize", "(I)I", NATIVE { return fromInt(specSize(asInt(a[0]))); });
    v->addStaticNative("resolveSize", "(II)I", NATIVE { return fromInt(resolveSize(asInt(a[0]), asInt(a[1]))); });

    // --- LayoutParams ----------------------------------------------------------
    Class* lp = vm.defineNativeClass("Landroid/view/ViewGroup$LayoutParams;", "Ljava/lang/Object;");
    lpf.width = lp->addField("width", "I");
    lpf.height = lp->addField("height", "I");
    lp->addNative("<init>", "(II)V", NATIVE {
        asObj(a[0])->fields[lpf.width->slot] = a[1];
        asObj(a[0])->fields[lpf.height->slot] = a[2];
        return 0;
    });
    Class* mlp = vm.defineNativeClass("Landroid/view/ViewGroup$MarginLayoutParams;", "Landroid/view/ViewGroup$LayoutParams;");
    lpf.leftMargin = mlp->addField("leftMargin", "I");
    lpf.topMargin = mlp->addField("topMargin", "I");
    lpf.rightMargin = mlp->addField("rightMargin", "I");
    lpf.bottomMargin = mlp->addField("bottomMargin", "I");
    mlp->addNative("setMargins", "(IIII)V", NATIVE {
        Object* o = asObj(a[0]);
        o->fields[lpf.leftMargin->slot] = a[1];
        o->fields[lpf.topMargin->slot] = a[2];
        o->fields[lpf.rightMargin->slot] = a[3];
        o->fields[lpf.bottomMargin->slot] = a[4];
        rt().requestLayout();
        return 0;
    });
    Class* llp = vm.defineNativeClass("Landroid/widget/LinearLayout$LayoutParams;", "Landroid/view/ViewGroup$MarginLayoutParams;");
    lpf.weight = llp->addField("weight", "F");
    lpf.gravity = llp->addField("gravity", "I");
    llp->addNative("<init>", "(II)V", NATIVE {
        asObj(a[0])->fields[lpf.width->slot] = a[1];
        asObj(a[0])->fields[lpf.height->slot] = a[2];
        asObj(a[0])->fields[lpf.gravity->slot] = fromInt(-1);
        return 0;
    });
    llp->addNative("<init>", "(IIF)V", NATIVE {
        asObj(a[0])->fields[lpf.width->slot] = a[1];
        asObj(a[0])->fields[lpf.height->slot] = a[2];
        asObj(a[0])->fields[lpf.weight->slot] = a[3];
        asObj(a[0])->fields[lpf.gravity->slot] = fromInt(-1);
        return 0;
    });
    Class* flp = vm.defineNativeClass("Landroid/widget/FrameLayout$LayoutParams;", "Landroid/view/ViewGroup$MarginLayoutParams;");
    lpf.frameGravity = flp->addField("gravity", "I");
    flp->addNative("<init>", "(II)V", NATIVE {
        asObj(a[0])->fields[lpf.width->slot] = a[1];
        asObj(a[0])->fields[lpf.height->slot] = a[2];
        asObj(a[0])->fields[lpf.frameGravity->slot] = fromInt(-1);
        return 0;
    });
    flp->addNative("<init>", "(III)V", NATIVE {
        asObj(a[0])->fields[lpf.width->slot] = a[1];
        asObj(a[0])->fields[lpf.height->slot] = a[2];
        asObj(a[0])->fields[lpf.frameGravity->slot] = a[3];
        return 0;
    });

    // --- ViewGroup ----------------------------------------------------------------
    Class* vg = vm.defineNativeClass("Landroid/view/ViewGroup;", "Landroid/view/View;", {"Landroid/view/ViewParent;"}, kPublic | kAbstract);
    auto groupInit = NATIVE { view(a[0])->context = asObj(a[1]); view(a[0])->isGroup = true; return 0; };
    vg->addNative("<init>", "(Landroid/content/Context;)V", groupInit);
    vg->addNative("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V", groupInit);
    auto addView = [](VM& vm, Reg self, Reg child, int index, Object* params) {
        Object* c = asObj(child);
        if (!c) vm.throwNPE("addView(null)");
        if (view(c)->parent) vm.throwNew("Ljava/lang/IllegalStateException;", "The specified child already has a parent.");
        auto& ch = view(self)->children;
        if (index < 0 || size_t(index) > ch.size()) ch.push_back(c);
        else ch.insert(ch.begin() + index, c);
        view(c)->parent = asObj(self);
        if (params) view(c)->layoutParams = params;
        rt().requestLayout();
    };
    vg->addNative("addView", "(Landroid/view/View;)V", NATIVE_CAPTURE(addView) { addView(vm, a[0], a[1], -1, nullptr); return 0; });
    vg->addNative("addView", "(Landroid/view/View;I)V", NATIVE_CAPTURE(addView) { addView(vm, a[0], a[1], asInt(a[2]), nullptr); return 0; });
    vg->addNative("addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V", NATIVE_CAPTURE(addView) {
        addView(vm, a[0], a[1], -1, asObj(a[2]));
        return 0;
    });
    vg->addNative("addView", "(Landroid/view/View;II)V", NATIVE_CAPTURE(addView) {
        Object* params = vm.construct(vm.findClass("Landroid/view/ViewGroup$LayoutParams;"), "(II)V", {a[2], a[3]});
        addView(vm, a[0], a[1], -1, params);
        return 0;
    });
    vg->addNative("removeView", "(Landroid/view/View;)V", NATIVE {
        auto& ch = view(a[0])->children;
        auto it = std::find(ch.begin(), ch.end(), asObj(a[1]));
        if (it != ch.end()) { view(*it)->parent = nullptr; ch.erase(it); }
        rt().requestLayout();
        return 0;
    });
    vg->addNative("removeAllViews", "()V", NATIVE {
        for (Object* c : view(a[0])->children) view(c)->parent = nullptr;
        view(a[0])->children.clear();
        rt().requestLayout();
        return 0;
    });
    vg->addNative("getChildCount", "()I", NATIVE { return fromInt(int32_t(view(a[0])->children.size())); });
    vg->addNative("getChildAt", "(I)Landroid/view/View;", NATIVE {
        auto& ch = view(a[0])->children;
        int i = asInt(a[1]);
        return i >= 0 && size_t(i) < ch.size() ? fromObj(ch[size_t(i)]) : 0;
    });

    Class* ll = vm.defineNativeClass("Landroid/widget/LinearLayout;", "Landroid/view/ViewGroup;");
    ll->addNative("setOrientation", "(I)V", NATIVE { view(a[0])->orientation = asInt(a[1]); rt().requestLayout(); return 0; });
    ll->addNative("getOrientation", "()I", NATIVE { return fromInt(view(a[0])->orientation); });
    ll->addNative("setGravity", "(I)V", NATIVE { view(a[0])->gravity = asInt(a[1]); rt().requestLayout(); return 0; });
    ll->addNative("onMeasure", "(II)V", NATIVE { measureLinear(asObj(a[0]), asInt(a[1]), asInt(a[2])); return 0; });
    ll->addNative("onLayout", "(ZIIII)V", NATIVE { layoutLinear(asObj(a[0])); return 0; });

    Class* fl = vm.defineNativeClass("Landroid/widget/FrameLayout;", "Landroid/view/ViewGroup;");
    fl->addNative("onMeasure", "(II)V", NATIVE { measureFrame(asObj(a[0]), asInt(a[1]), asInt(a[2])); return 0; });
    fl->addNative("onLayout", "(ZIIII)V", NATIVE { layoutFrame(asObj(a[0])); return 0; });

    Class* sv = vm.defineNativeClass("Landroid/widget/ScrollView;", "Landroid/widget/FrameLayout;");
    auto scrollInit = NATIVE { view(a[0])->context = asObj(a[1]); view(a[0])->isGroup = view(a[0])->isScroll = true; return 0; };
    sv->addNative("<init>", "(Landroid/content/Context;)V", scrollInit);
    sv->addNative("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V", scrollInit);
    sv->addNative("scrollTo", "(II)V", NATIVE { view(a[0])->scrollY = asInt(a[2]); layoutFrame(asObj(a[0])); rt().invalidate(); return 0; });
    sv->addNative("getScrollY", "()I", NATIVE { return fromInt(view(a[0])->scrollY); });
    sv->addNative("fullScroll", "(I)Z", NATIVE {
        view(a[0])->scrollY = asInt(a[1]) == 130 ? 1 << 29 : 0;  // FOCUS_DOWN = 130
        rt().requestLayout();
        return 1;
    });

    // --- TextView, Button, EditText ----------------------------------------------
    Class* tv = vm.defineNativeClass("Landroid/widget/TextView;", "Landroid/view/View;");
    auto textInit = [](Widget w) {
        return NATIVE_CAPTURE(w) {
            ViewPeer* p = view(a[0]);
            p->context = asObj(a[1]);
            p->widget = w;
            p->textSize = dp(14);
            if (w == Widget::Button) {
                p->allCaps = true;
                p->gravity = 0x11;  // CENTER
                p->padL = p->padR = int(dp(16));
                p->padT = p->padB = int(dp(10));
                p->minW = int(dp(88));
                p->minH = int(dp(48));
                p->clickable = true;
            } else if (w == Widget::EditText) {
                p->textSize = dp(18);
                p->gravity = 0x13;  // LEFT | CENTER_VERTICAL
                p->padL = p->padR = int(dp(4));
                p->padT = p->padB = int(dp(10));
                p->minH = int(dp(48));
                p->clickable = true;
            }
            return 0;
        };
    };
    for (auto [desc, super, w] : {std::tuple{"Landroid/widget/TextView;", "", Widget::TextView},
                                  std::tuple{"Landroid/widget/Button;", "Landroid/widget/TextView;", Widget::Button},
                                  std::tuple{"Landroid/widget/EditText;", "Landroid/widget/TextView;", Widget::EditText}}) {
        Class* c = std::string(super).empty() ? tv : vm.defineNativeClass(desc, super);
        c->addNative("<init>", "(Landroid/content/Context;)V", textInit(w));
        c->addNative("<init>", "(Landroid/content/Context;Landroid/util/AttributeSet;)V", textInit(w));
    }
    tv->addNative("setText", "(Ljava/lang/CharSequence;)V", NATIVE {
        view(a[0])->text = asObj(a[1]) ? vm.string16Of(asObj(a[1])) : u"";
        rt().requestLayout();
        return 0;
    });
    tv->addNative("setText", "(I)V", NATIVE { vm.warnOnce("TextView.setText(int resId): resources are not supported yet"); return 0; });
    tv->addNative("append", "(Ljava/lang/CharSequence;)V", NATIVE {
        view(a[0])->text += vm.string16Of(asObj(a[1]));
        rt().requestLayout();
        return 0;
    });
    tv->addNative("getText", "()Ljava/lang/CharSequence;", NATIVE { return newStr(vm, view(a[0])->text); });
    vm.findClass("Landroid/widget/EditText;")->addNative("getText", "()Landroid/text/Editable;", NATIVE {
        return newStr(vm, view(a[0])->text);
    });
    tv->addNative("setHint", "(Ljava/lang/CharSequence;)V", NATIVE { view(a[0])->hint = vm.string16Of(asObj(a[1])); rt().invalidate(); return 0; });
    tv->addNative("setTextSize", "(F)V", NATIVE { view(a[0])->textSize = dp(asFloat(a[1])); rt().requestLayout(); return 0; });
    tv->addNative("setTextSize", "(IF)V", NATIVE {
        // TypedValue units: 0 px, 1 dp, 2 sp (we treat sp like dp).
        view(a[0])->textSize = asInt(a[1]) == 0 ? asFloat(a[2]) : dp(asFloat(a[2]));
        rt().requestLayout();
        return 0;
    });
    tv->addNative("getTextSize", "()F", NATIVE { return fromFloat(view(a[0])->textSize); });
    tv->addNative("setTextColor", "(I)V", NATIVE { view(a[0])->textColor = uint32_t(asInt(a[1])); rt().invalidate(); return 0; });
    tv->addNative("setGravity", "(I)V", NATIVE { view(a[0])->gravity = asInt(a[1]); rt().invalidate(); return 0; });
    tv->addNative("setAllCaps", "(Z)V", NATIVE { view(a[0])->allCaps = asInt(a[1]); rt().requestLayout(); return 0; });
    tv->addNative("onMeasure", "(II)V", NATIVE {
        ViewPeer* p = view(a[0]);
        int ws = asInt(a[1]), hs = asInt(a[2]);
        float maxText = specMode(ws) == UNSPECIFIED ? 0.f : float(specSize(ws) - p->padL - p->padR);
        std::u16string text = p->text.empty() ? p->hint : displayText(p);
        auto lines = wrapText(text, p->textSize, maxText);
        float w = 0;
        for (auto& l : lines) w = std::max(w, Raster::textWidth(l, p->textSize));
        int desiredW = int(std::ceil(w)) + p->padL + p->padR;
        int desiredH = int(std::ceil(Raster::lineHeight(p->textSize) * float(lines.size()))) + p->padT + p->padB;
        setMeasured(asObj(a[0]), resolveSize(std::max(desiredW, p->minW), ws), resolveSize(std::max(desiredH, p->minH), hs));
        return 0;
    });
    tv->addNative("onDraw", "(Landroid/graphics/Canvas;)V", NATIVE { drawTextView(asObj(a[0])); return 0; });

    // --- MotionEvent ------------------------------------------------------------
    Class* me = vm.defineNativeClass("Landroid/view/MotionEvent;", "Ljava/lang/Object;");
    me->peerFactory = [] { return std::make_unique<MotionEventPeer>(); };
    auto mev = [](Reg r) { return asObj(r)->peerAs<MotionEventPeer>(); };
    me->addNative("getX", "()F", NATIVE_CAPTURE(mev) { return fromFloat(mev(a[0])->x); });
    me->addNative("getY", "()F", NATIVE_CAPTURE(mev) { return fromFloat(mev(a[0])->y); });
    me->addNative("getRawX", "()F", NATIVE_CAPTURE(mev) { return fromFloat(mev(a[0])->rawX); });
    me->addNative("getRawY", "()F", NATIVE_CAPTURE(mev) { return fromFloat(mev(a[0])->rawY); });
    me->addNative("getAction", "()I", NATIVE_CAPTURE(mev) { return fromInt(mev(a[0])->action); });
    me->addNative("getActionMasked", "()I", NATIVE_CAPTURE(mev) { return fromInt(mev(a[0])->action); });
    me->addNative("getEventTime", "()J", NATIVE_CAPTURE(mev) { return fromLong(mev(a[0])->time); });
    me->addNative("getPointerCount", "()I", NATIVE { return fromInt(1); });
}

void registerGraphics(VM& vm) {
    // --- Color ------------------------------------------------------------------
    Class* color = vm.defineNativeClass("Landroid/graphics/Color;", "Ljava/lang/Object;");
    color->addStaticNative("rgb", "(III)I", NATIVE {
        return fromInt(int32_t(0xff000000u | (uint32_t(asInt(a[0]) & 0xff) << 16) | (uint32_t(asInt(a[1]) & 0xff) << 8) | uint32_t(asInt(a[2]) & 0xff)));
    });
    color->addStaticNative("argb", "(IIII)I", NATIVE {
        return fromInt(int32_t((uint32_t(asInt(a[0]) & 0xff) << 24) | (uint32_t(asInt(a[1]) & 0xff) << 16) |
                               (uint32_t(asInt(a[2]) & 0xff) << 8) | uint32_t(asInt(a[3]) & 0xff)));
    });
    color->addStaticNative("alpha", "(I)I", NATIVE { return fromInt((uint32_t(asInt(a[0])) >> 24) & 0xff); });
    color->addStaticNative("red", "(I)I", NATIVE { return fromInt((uint32_t(asInt(a[0])) >> 16) & 0xff); });
    color->addStaticNative("green", "(I)I", NATIVE { return fromInt((uint32_t(asInt(a[0])) >> 8) & 0xff); });
    color->addStaticNative("blue", "(I)I", NATIVE { return fromInt(uint32_t(asInt(a[0])) & 0xff); });
    color->addStaticNative("parseColor", "(Ljava/lang/String;)I", NATIVE {
        std::string s = utf16ToUtf8(str(vm, a[0]));
        static const std::map<std::string, uint32_t> named = {
            {"red", 0xffff0000}, {"green", 0xff00ff00}, {"blue", 0xff0000ff}, {"black", 0xff000000},
            {"white", 0xffffffff}, {"gray", 0xff888888}, {"grey", 0xff888888}, {"yellow", 0xffffff00},
            {"cyan", 0xff00ffff}, {"magenta", 0xffff00ff}, {"transparent", 0}};
        if (auto it = named.find(s); it != named.end()) return fromInt(int32_t(it->second));
        if (s.size() == 7 || s.size() == 9) {
            if (s[0] == '#') {
                uint32_t v = uint32_t(std::stoul(s.substr(1), nullptr, 16));
                if (s.size() == 7) v |= 0xff000000;
                return fromInt(int32_t(v));
            }
        }
        vm.throwNew("Ljava/lang/IllegalArgumentException;", "Unknown color: " + s);
    });

    // --- Paint ------------------------------------------------------------------
    defineEnum(vm, "Landroid/graphics/Paint$Style;");
    defineEnum(vm, "Landroid/graphics/Paint$Align;");
    Class* paint = vm.defineNativeClass("Landroid/graphics/Paint;", "Ljava/lang/Object;");
    paint->peerFactory = [] { return std::make_unique<PaintPeer>(); };
    auto pp = [](Reg r) { return asObj(r)->peerAs<PaintPeer>(); };
    paint->addNative("<init>", "()V", NATIVE { return 0; });
    paint->addNative("<init>", "(I)V", NATIVE { return 0; });
    paint->addNative("<init>", "(Landroid/graphics/Paint;)V", NATIVE_CAPTURE(pp) { *pp(a[0]) = *pp(a[1]); return 0; });
    paint->addNative("setColor", "(I)V", NATIVE_CAPTURE(pp) { pp(a[0])->color = uint32_t(asInt(a[1])); return 0; });
    paint->addNative("getColor", "()I", NATIVE_CAPTURE(pp) { return fromInt(int32_t(pp(a[0])->color)); });
    paint->addNative("setARGB", "(IIII)V", NATIVE_CAPTURE(pp) {
        pp(a[0])->color = (uint32_t(asInt(a[1]) & 0xff) << 24) | (uint32_t(asInt(a[2]) & 0xff) << 16) |
                          (uint32_t(asInt(a[3]) & 0xff) << 8) | uint32_t(asInt(a[4]) & 0xff);
        return 0;
    });
    paint->addNative("setAlpha", "(I)V", NATIVE_CAPTURE(pp) {
        pp(a[0])->color = (pp(a[0])->color & 0x00ffffff) | (uint32_t(asInt(a[1]) & 0xff) << 24);
        return 0;
    });
    paint->addNative("setStrokeWidth", "(F)V", NATIVE_CAPTURE(pp) { pp(a[0])->strokeWidth = asFloat(a[1]); return 0; });
    paint->addNative("getStrokeWidth", "()F", NATIVE_CAPTURE(pp) { return fromFloat(pp(a[0])->strokeWidth); });
    paint->addNative("setTextSize", "(F)V", NATIVE_CAPTURE(pp) { pp(a[0])->textSize = asFloat(a[1]); return 0; });
    paint->addNative("getTextSize", "()F", NATIVE_CAPTURE(pp) { return fromFloat(pp(a[0])->textSize); });
    paint->addNative("setStyle", "(Landroid/graphics/Paint$Style;)V", NATIVE_CAPTURE(pp) { pp(a[0])->style = enumOrdinal(asObj(a[1])); return 0; });
    paint->addNative("setTextAlign", "(Landroid/graphics/Paint$Align;)V", NATIVE_CAPTURE(pp) { pp(a[0])->align = enumOrdinal(asObj(a[1])); return 0; });
    paint->addNative("setAntiAlias", "(Z)V", NATIVE { return 0; });  // we always anti-alias
    paint->addNative("setFlags", "(I)V", NATIVE { return 0; });
    paint->addNative("measureText", "(Ljava/lang/String;)F", NATIVE_CAPTURE(pp) {
        return fromFloat(Raster::textWidth(str(vm, a[1]), pp(a[0])->textSize));
    });
    paint->addNative("ascent", "()F", NATIVE_CAPTURE(pp) { return fromFloat(-Raster::ascent(pp(a[0])->textSize)); });
    paint->addNative("descent", "()F", NATIVE_CAPTURE(pp) { return fromFloat(Raster::descent(pp(a[0])->textSize)); });

    // --- RectF / Rect -------------------------------------------------------------
    for (auto [desc, t] : {std::pair{"Landroid/graphics/RectF;", "F"}, std::pair{"Landroid/graphics/Rect;", "I"}}) {
        Class* c = vm.defineNativeClass(desc, "Ljava/lang/Object;");
        for (const char* f : {"left", "top", "right", "bottom"}) c->addField(f, t);
        std::string T = t;
        c->addNative("<init>", "()V", NATIVE { return 0; });
        auto set = NATIVE { for (int i = 0; i < 4; ++i) asObj(a[0])->fields[size_t(i)] = a[1 + i]; return 0; };
        c->addNative("<init>", "(" + T + T + T + T + ")V", set);
        c->addNative("set", "(" + T + T + T + T + ")V", set);
        bool isF = T == "F";
        auto num = [isF](Reg r) { return isF ? double(asFloat(r)) : double(asInt(r)); };
        auto mk = [isF](double v) { return isF ? fromFloat(float(v)) : fromInt(int32_t(v)); };
        c->addNative("width", "()" + T, NATIVE_CAPTURE(num, mk) { auto& f = asObj(a[0])->fields; return mk(num(f[2]) - num(f[0])); });
        c->addNative("height", "()" + T, NATIVE_CAPTURE(num, mk) { auto& f = asObj(a[0])->fields; return mk(num(f[3]) - num(f[1])); });
        std::string centerT = isF ? "F" : "I";
        c->addNative("centerX", "()" + centerT, NATIVE_CAPTURE(num, mk) { auto& f = asObj(a[0])->fields; return mk((num(f[0]) + num(f[2])) / 2); });
        c->addNative("centerY", "()" + centerT, NATIVE_CAPTURE(num, mk) { auto& f = asObj(a[0])->fields; return mk((num(f[1]) + num(f[3])) / 2); });
        c->addNative("contains", "(" + T + T + ")Z", NATIVE_CAPTURE(num) {
            auto& f = asObj(a[0])->fields;
            double x = num(a[1]), y = num(a[2]);
            return x >= num(f[0]) && x < num(f[2]) && y >= num(f[1]) && y < num(f[3]);
        });
    }

    // --- Canvas: every call draws straight into the runtime's framebuffer ---------
    Class* canvas = vm.defineNativeClass("Landroid/graphics/Canvas;", "Ljava/lang/Object;");
    auto rect4 = [](Reg* a) { return std::array<float, 4>{asFloat(a[0]), asFloat(a[1]), asFloat(a[2]), asFloat(a[3])}; };
    auto rectF = [](Object* r) {
        if (!r) return std::array<float, 4>{0, 0, 0, 0};
        return std::array<float, 4>{asFloat(r->fields[0]), asFloat(r->fields[1]), asFloat(r->fields[2]), asFloat(r->fields[3])};
    };
    auto paintOf = [](VM& vm, Reg r) {
        Object* p = asObj(r);
        if (!p) vm.throwNPE("Paint is null");
        return p->peerAs<PaintPeer>();
    };
    // Draws one shape in the paint's style (fill, stroke or both).
    auto shape = [](PaintPeer* p, const std::function<void(bool stroke, float width)>& draw) {
        if (p->style == 0 || p->style == 2) draw(false, 0);
        if (p->style == 1 || p->style == 2) draw(true, p->strokeWidth);
    };
    canvas->addNative("getWidth", "()I", NATIVE { return fromInt(rt().raster().clipBounds().right - rt().raster().clipBounds().left); });
    canvas->addNative("getHeight", "()I", NATIVE { return fromInt(rt().raster().clipBounds().bottom - rt().raster().clipBounds().top); });
    canvas->addNative("drawColor", "(I)V", NATIVE { rt().raster().fill(uint32_t(asInt(a[1]))); return 0; });
    canvas->addNative("drawRGB", "(III)V", NATIVE {
        rt().raster().fill(0xff000000u | (uint32_t(asInt(a[1])) << 16) | (uint32_t(asInt(a[2])) << 8) | uint32_t(asInt(a[3])));
        return 0;
    });
    canvas->addNative("drawARGB", "(IIII)V", NATIVE {
        rt().raster().fill((uint32_t(asInt(a[1])) << 24) | (uint32_t(asInt(a[2])) << 16) | (uint32_t(asInt(a[3])) << 8) | uint32_t(asInt(a[4])));
        return 0;
    });
    canvas->addNative("drawPaint", "(Landroid/graphics/Paint;)V", NATIVE_CAPTURE(paintOf) { rt().raster().fill(paintOf(vm, a[1])->color); return 0; });
    auto drawRect = [shape](PaintPeer* p, std::array<float, 4> r) {
        shape(p, [&](bool stroke, float w) {
            if (stroke) rt().raster().strokeRect(r[0], r[1], r[2], r[3], std::max(w, 1.f), p->color);
            else rt().raster().fillRect(r[0], r[1], r[2], r[3], p->color);
        });
    };
    canvas->addNative("drawRect", "(FFFFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(drawRect, rect4, paintOf) {
        drawRect(paintOf(vm, a[5]), rect4(a + 1));
        return 0;
    });
    canvas->addNative("drawRect", "(Landroid/graphics/RectF;Landroid/graphics/Paint;)V", NATIVE_CAPTURE(drawRect, rectF, paintOf) {
        drawRect(paintOf(vm, a[2]), rectF(asObj(a[1])));
        return 0;
    });
    canvas->addNative("drawRect", "(Landroid/graphics/Rect;Landroid/graphics/Paint;)V", NATIVE_CAPTURE(drawRect, paintOf) {
        Object* r = asObj(a[1]);
        drawRect(paintOf(vm, a[2]), {float(asInt(r->fields[0])), float(asInt(r->fields[1])), float(asInt(r->fields[2])), float(asInt(r->fields[3]))});
        return 0;
    });
    auto drawRound = [shape](PaintPeer* p, std::array<float, 4> r, float radius) {
        shape(p, [&](bool stroke, float w) {
            if (stroke) rt().raster().strokeRoundRect(r[0], r[1], r[2], r[3], radius, std::max(w, 1.f), p->color);
            else rt().raster().fillRoundRect(r[0], r[1], r[2], r[3], radius, p->color);
        });
    };
    canvas->addNative("drawRoundRect", "(FFFFFFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(drawRound, rect4, paintOf) {
        drawRound(paintOf(vm, a[7]), rect4(a + 1), asFloat(a[5]));
        return 0;
    });
    canvas->addNative("drawRoundRect", "(Landroid/graphics/RectF;FFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(drawRound, rectF, paintOf) {
        drawRound(paintOf(vm, a[4]), rectF(asObj(a[1])), asFloat(a[2]));
        return 0;
    });
    auto drawOval = [shape](PaintPeer* p, std::array<float, 4> r) {
        shape(p, [&](bool stroke, float w) {
            if (stroke) rt().raster().strokeOval(r[0], r[1], r[2], r[3], std::max(w, 1.f), p->color);
            else rt().raster().fillOval(r[0], r[1], r[2], r[3], p->color);
        });
    };
    canvas->addNative("drawCircle", "(FFFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(drawOval, paintOf) {
        float x = asFloat(a[1]), y = asFloat(a[2]), r = asFloat(a[3]);
        drawOval(paintOf(vm, a[4]), {x - r, y - r, x + r, y + r});
        return 0;
    });
    canvas->addNative("drawOval", "(FFFFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(drawOval, rect4, paintOf) {
        drawOval(paintOf(vm, a[5]), rect4(a + 1));
        return 0;
    });
    canvas->addNative("drawOval", "(Landroid/graphics/RectF;Landroid/graphics/Paint;)V", NATIVE_CAPTURE(drawOval, rectF, paintOf) {
        drawOval(paintOf(vm, a[2]), rectF(asObj(a[1])));
        return 0;
    });
    canvas->addNative("drawLine", "(FFFFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(paintOf) {
        PaintPeer* p = paintOf(vm, a[5]);
        rt().raster().line(asFloat(a[1]), asFloat(a[2]), asFloat(a[3]), asFloat(a[4]), p->strokeWidth, p->color);
        return 0;
    });
    canvas->addNative("drawPoint", "(FFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(paintOf) {
        PaintPeer* p = paintOf(vm, a[3]);
        float h = std::max(p->strokeWidth, 1.f) / 2;
        rt().raster().fillRect(asFloat(a[1]) - h, asFloat(a[2]) - h, asFloat(a[1]) + h, asFloat(a[2]) + h, p->color);
        return 0;
    });
    canvas->addNative("drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V", NATIVE_CAPTURE(paintOf) {
        PaintPeer* p = paintOf(vm, a[4]);
        const std::u16string& s = str(vm, a[1]);
        float x = asFloat(a[2]);
        if (p->align == 1) x -= Raster::textWidth(s, p->textSize) / 2;
        if (p->align == 2) x -= Raster::textWidth(s, p->textSize);
        rt().raster().text(s, x, asFloat(a[3]), p->textSize, p->color);
        return 0;
    });
    canvas->addNative("save", "()I", NATIVE { rt().raster().save(); return fromInt(rt().raster().saveCount()); });
    canvas->addNative("restore", "()V", NATIVE { rt().raster().restore(); return 0; });
    canvas->addNative("translate", "(FF)V", NATIVE { rt().raster().translate(asFloat(a[1]), asFloat(a[2])); return 0; });
    canvas->addNative("scale", "(FF)V", NATIVE { rt().raster().scale(asFloat(a[1]), asFloat(a[2])); return 0; });
    canvas->addNative("clipRect", "(FFFF)Z", NATIVE { rt().raster().clipRect(asFloat(a[1]), asFloat(a[2]), asFloat(a[3]), asFloat(a[4])); return 1; });
}

void registerApp(VM& vm) {
    // --- Context and Activity -----------------------------------------------------
    Class* ctx = vm.defineNativeClass("Landroid/content/Context;", "Ljava/lang/Object;", {}, kPublic | kAbstract);
    vm.defineNativeClass("Landroid/content/ContextWrapper;", "Landroid/content/Context;");
    vm.defineNativeClass("Landroid/view/ContextThemeWrapper;", "Landroid/content/ContextWrapper;");
    Class* act = vm.defineNativeClass("Landroid/app/Activity;", "Landroid/view/ContextThemeWrapper;");
    act->peerFactory = [] { return std::make_unique<ActivityPeer>(); };
    // High-level emulation: AndroidX activities are huge libraries bundled in
    // the APK. Defining them natively makes our simple Activity take their place.
    for (const char* d : {"Landroidx/core/app/ComponentActivity;", "Landroidx/activity/ComponentActivity;",
                          "Landroidx/fragment/app/FragmentActivity;", "Landroidx/appcompat/app/AppCompatActivity;"})
        vm.defineNativeClass(d, d == std::string("Landroidx/core/app/ComponentActivity;") ? "Landroid/app/Activity;"
                                : d == std::string("Landroidx/activity/ComponentActivity;") ? "Landroidx/core/app/ComponentActivity;"
                                : d == std::string("Landroidx/fragment/app/FragmentActivity;") ? "Landroidx/activity/ComponentActivity;"
                                                                                                 : "Landroidx/fragment/app/FragmentActivity;");
    auto ap = [](Reg r) { return asObj(r)->peerAs<ActivityPeer>(); };
    act->addNative("<init>", "()V", NATIVE { return 0; });
    for (const char* cb : {"onStart", "onResume", "onPause", "onStop", "onDestroy", "onRestart"})
        act->addNative(cb, "()V", NATIVE { return 0; });
    act->addNative("onCreate", "(Landroid/os/Bundle;)V", NATIVE { return 0; });
    act->addNative("setContentView", "(Landroid/view/View;)V", NATIVE_CAPTURE(ap) {
        ap(a[0])->contentView = asObj(a[1]);
        rt().requestLayout();
        return 0;
    });
    act->addNative("setContentView", "(I)V", NATIVE {
        vm.warnOnce("setContentView(R.layout...): XML layouts are not supported yet - build the UI in code");
        return 0;
    });
    act->addNative("findViewById", "(I)Landroid/view/View;", NATIVE_CAPTURE(ap) {
        Object* root = ap(a[0])->contentView;
        return root ? vm.callVirtual(root, "findViewById", "(I)Landroid/view/View;", {a[1]}) : 0;
    });
    act->addNative("setTitle", "(Ljava/lang/CharSequence;)V", NATIVE_CAPTURE(ap) {
        ap(a[0])->title = vm.string16Of(asObj(a[1]));
        rt().invalidate();
        return 0;
    });
    act->addNative("getTitle", "()Ljava/lang/CharSequence;", NATIVE_CAPTURE(ap) { return newStr(vm, ap(a[0])->title); });
    act->addNative("finish", "()V", NATIVE { rt().finishActivity(asObj(a[0])); return 0; });
    act->addNative("isFinishing", "()Z", NATIVE_CAPTURE(ap) { return ap(a[0])->finishing; });
    act->addNative("onBackPressed", "()V", NATIVE { rt().finishActivity(asObj(a[0])); return 0; });
    act->addNative("getIntent", "()Landroid/content/Intent;", NATIVE_CAPTURE(ap) {
        if (!ap(a[0])->intent) ap(a[0])->intent = vm.construct(vm.findClass("Landroid/content/Intent;"));
        return fromObj(ap(a[0])->intent);
    });
    act->addNative("runOnUiThread", "(Ljava/lang/Runnable;)V", NATIVE { vm.callVirtual(asObj(a[1]), "run", "()V"); return 0; });
    ctx->addNative("startActivity", "(Landroid/content/Intent;)V", NATIVE { rt().startActivity(asObj(a[1])); return 0; });
    ctx->addNative("getApplicationContext", "()Landroid/content/Context;", NATIVE { return a[0]; });
    ctx->addNative("getPackageName", "()Ljava/lang/String;", NATIVE { return newStr(vm, std::string("app")); });

    // Resources: only display metrics for now (no resources.arsc parsing yet).
    Class* dm = vm.defineNativeClass("Landroid/util/DisplayMetrics;", "Ljava/lang/Object;");
    dm->addField("density", "F");
    dm->addField("scaledDensity", "F");
    dm->addField("widthPixels", "I");
    dm->addField("heightPixels", "I");
    dm->addField("densityDpi", "I");
    Class* res = vm.defineNativeClass("Landroid/content/res/Resources;", "Ljava/lang/Object;");
    static Object* resources = nullptr;
    static Object* metrics = nullptr;
    ctx->addNative("getResources", "()Landroid/content/res/Resources;", NATIVE_CAPTURE(res, dm) {
        if (!resources) {
            resources = vm.alloc(res);
            metrics = vm.alloc(dm);
            vm.addRoot(resources);
            vm.addRoot(metrics);
            const Screen& s = rt().screen();
            metrics->fields[dm->findField("density")->slot] = fromFloat(s.density);
            metrics->fields[dm->findField("scaledDensity")->slot] = fromFloat(s.density);
            metrics->fields[dm->findField("widthPixels")->slot] = fromInt(s.width);
            metrics->fields[dm->findField("heightPixels")->slot] = fromInt(s.height);
            metrics->fields[dm->findField("densityDpi")->slot] = fromInt(int(s.density * 160));
        }
        return fromObj(resources);
    });
    res->addNative("getDisplayMetrics", "()Landroid/util/DisplayMetrics;", NATIVE { return fromObj(metrics); });
    Class* tval = vm.defineNativeClass("Landroid/util/TypedValue;", "Ljava/lang/Object;");
    tval->addStaticNative("applyDimension", "(IFLandroid/util/DisplayMetrics;)F", NATIVE {
        float v = asFloat(a[1]);
        return fromFloat(asInt(a[0]) == 0 ? v : v * rt().screen().density);
    });

    // --- Intent and Bundle: a bag of extras --------------------------------------
    for (const char* d : {"Landroid/content/Intent;", "Landroid/os/Bundle;"}) {
        Class* c = vm.defineNativeClass(d, "Ljava/lang/Object;");
        c->peerFactory = [] { return std::make_unique<ExtrasPeer>(); };
        c->addNative("<init>", "()V", NATIVE { return 0; });
        std::string self = d;
        bool isIntent = self == "Landroid/content/Intent;";
        std::string ret = isIntent ? self : "V";
        std::string put = isIntent ? "putExtra" : "putString";
        auto ep = [](Reg r) { return asObj(r)->peerAs<ExtrasPeer>(); };
        c->addNative(put, "(Ljava/lang/String;Ljava/lang/String;)" + ret, NATIVE_CAPTURE(ep, isIntent) {
            ep(a[0])->objects[str(vm, a[1])] = asObj(a[2]);
            return isIntent ? a[0] : 0;
        });
        c->addNative(isIntent ? "putExtra" : "putInt", "(Ljava/lang/String;I)" + ret, NATIVE_CAPTURE(ep, isIntent) {
            ep(a[0])->values[str(vm, a[1])] = a[2];
            return isIntent ? a[0] : 0;
        });
        c->addNative(isIntent ? "getStringExtra" : "getString", "(Ljava/lang/String;)Ljava/lang/String;", NATIVE_CAPTURE(ep) {
            auto& m = ep(a[0])->objects;
            auto it = m.find(str(vm, a[1]));
            return it == m.end() ? 0 : fromObj(it->second);
        });
        c->addNative(isIntent ? "getIntExtra" : "getInt", "(Ljava/lang/String;I)I", NATIVE_CAPTURE(ep) {
            auto& m = ep(a[0])->values;
            auto it = m.find(str(vm, a[1]));
            return it == m.end() ? a[2] : it->second;
        });
        if (isIntent) {
            c->addNative("<init>", "(Landroid/content/Context;Ljava/lang/Class;)V", NATIVE_CAPTURE(ep) {
                ep(a[0])->target = VM::classFromMirror(asObj(a[2]));
                return 0;
            });
        } else {
            c->addNative("getInt", "(Ljava/lang/String;)I", NATIVE_CAPTURE(ep) {
                auto& m = ep(a[0])->values;
                auto it = m.find(str(vm, a[1]));
                return it == m.end() ? 0 : it->second;
            });
        }
    }

    // --- Handler, Looper, SystemClock ------------------------------------------
    Class* looper = vm.defineNativeClass("Landroid/os/Looper;", "Ljava/lang/Object;");
    looper->addStaticNative("getMainLooper", "()Landroid/os/Looper;", NATIVE_CAPTURE(looper) {
        static Object* main = nullptr;
        if (!main) { main = vm.alloc(looper); vm.addRoot(main); }
        return fromObj(main);
    });
    looper->addStaticNative("myLooper", "()Landroid/os/Looper;", NATIVE_CAPTURE(looper) {
        return vm.callStatic(looper, "getMainLooper", "()Landroid/os/Looper;");
    });
    Class* handler = vm.defineNativeClass("Landroid/os/Handler;", "Ljava/lang/Object;");
    handler->addNative("<init>", "()V", NATIVE { return 0; });
    handler->addNative("<init>", "(Landroid/os/Looper;)V", NATIVE { return 0; });
    handler->addNative("post", "(Ljava/lang/Runnable;)Z", NATIVE { rt().post(asObj(a[1]), 0); return 1; });
    handler->addNative("postDelayed", "(Ljava/lang/Runnable;J)Z", NATIVE { rt().post(asObj(a[1]), asLong(a[2])); return 1; });
    handler->addNative("removeCallbacks", "(Ljava/lang/Runnable;)V", NATIVE { rt().removeCallbacks(asObj(a[1])); return 0; });
    handler->addNative("removeCallbacksAndMessages", "(Ljava/lang/Object;)V", NATIVE {
        vm.warnOnce("Handler.removeCallbacksAndMessages is a no-op");
        return 0;
    });
    Class* clock = vm.defineNativeClass("Landroid/os/SystemClock;", "Ljava/lang/Object;");
    clock->addStaticNative("uptimeMillis", "()J", NATIVE { return fromLong(rt().now()); });
    clock->addStaticNative("elapsedRealtime", "()J", NATIVE { return fromLong(rt().now()); });

    // --- Log and Toast ------------------------------------------------------------
    Class* log = vm.defineNativeClass("Landroid/util/Log;", "Ljava/lang/Object;");
    for (char level : std::string("VDIWE")) {
        std::string name(1, char(std::tolower(level)));
        auto fn = NATIVE_CAPTURE(level) {
            std::cerr << level << "/" << VM::utf8(asObj(a[0])) << ": " << VM::utf8(asObj(a[1])) << "\n";
            return 0;
        };
        log->addStaticNative(name, "(Ljava/lang/String;Ljava/lang/String;)I", fn);
        log->addStaticNative(name, "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", fn);
    }
    Class* toast = vm.defineNativeClass("Landroid/widget/Toast;", "Ljava/lang/Object;");
    toast->peerFactory = [] { return std::make_unique<ToastPeer>(); };
    toast->addStaticNative("makeText", "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;", NATIVE_CAPTURE(toast) {
        Object* t = vm.alloc(toast);
        t->peerAs<ToastPeer>()->text = vm.string16Of(asObj(a[1]));
        t->peerAs<ToastPeer>()->duration = asInt(a[2]);
        return fromObj(t);
    });
    toast->addNative("show", "()V", NATIVE {
        auto* p = asObj(a[0])->peerAs<ToastPeer>();
        rt().showToast(p->text, p->duration == 1 ? 3500 : 2000);  // LENGTH_LONG / LENGTH_SHORT
        return 0;
    });
    toast->addNative("setText", "(Ljava/lang/CharSequence;)V", NATIVE {
        asObj(a[0])->peerAs<ToastPeer>()->text = vm.string16Of(asObj(a[1]));
        return 0;
    });
}

}  // namespace

void registerAndroid(VM& vm) {
    registerApp(vm);
    registerViews(vm);
    registerGraphics(vm);
}

void initAndroidStatics(VM& vm) {
    // Enum constants of Paint.Style and Paint.Align.
    auto makeEnum = [&](const std::string& desc, std::vector<std::string> names) {
        Class* c = vm.findClass(desc);
        ArrayObject* values = vm.newArray("[" + desc, int32_t(names.size()));
        for (size_t i = 0; i < names.size(); ++i) {
            Object* e = vm.alloc(c);
            e->fields[c->findField("name")->slot] = fromObj(vm.newString(names[i]));
            e->fields[c->findField("ordinal")->slot] = fromInt(int32_t(i));
            c->addStaticField(names[i], desc, fromObj(e));
            values->data[i] = fromObj(e);
        }
        Field* all = c->addStaticField("$VALUES", "[" + desc, fromObj(values));
        c->addStaticNative("values", "()[" + desc, NATIVE_CAPTURE(all) {
            auto* src = static_cast<ArrayObject*>(asObj(all->staticValue));
            ArrayObject* copy = vm.newArray(src->klass, int32_t(src->data.size()));
            copy->data = src->data;
            return fromObj(copy);
        });
    };
    makeEnum("Landroid/graphics/Paint$Style;", {"FILL", "STROKE", "FILL_AND_STROKE"});
    makeEnum("Landroid/graphics/Paint$Align;", {"LEFT", "CENTER", "RIGHT"});
}

}  // namespace droid
