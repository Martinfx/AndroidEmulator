// droidvm - runs simple Android apps (APK files) on a desktop, from scratch.
//
//   droidvm app.apk                       open a window and run the app
//   droidvm app.apk --headless --run 2000 --tap 360,700@500 --screenshot out.bmp
//   droidvm classes.dex --main com.example.Hello     run a plain Java main()
#include <SDL.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "apk/axml.h"
#include "apk/zip.h"
#include "framework/android.h"
#include "vm/vm.h"

using namespace droid;

namespace {

struct Options {
    std::string input;
    std::string activity;   // override the launcher activity
    std::string mainClass;  // run static main(String[]) instead of an activity
    bool headless = false;
    int64_t runMs = 3000;   // headless: how long to run
    std::string screenshot;
    struct Tap { int64_t at; float x, y; };
    std::vector<Tap> taps;
    struct Swipe { int64_t at; float x0, y0, x1, y1; };
    std::vector<Swipe> swipes;
    std::vector<std::pair<int64_t, std::string>> typed;
    float scale = 0.5f;
    bool trace = false;
};

void usage() {
    std::cerr <<
        "usage: droidvm <app.apk | classes.dex> [options]\n"
        "  --activity NAME     launch this activity instead of the manifest's launcher\n"
        "  --main CLASS        run CLASS.main(String[]) (console programs)\n"
        "  --scale F           window scale (default 0.5 of a 720x1280 screen)\n"
        "  --headless          no window; use with:\n"
        "    --run MS          run for MS milliseconds of virtual time (default 3000)\n"
        "    --tap X,Y@MS      tap at screen pixel X,Y at time MS (repeatable)\n"
        "    --swipe X0,Y0,X1,Y1@MS  drag from one point to another starting at time MS\n"
        "    --type TEXT@MS    type TEXT into the focused field at time MS\n"
        "    --screenshot F    save the final screen as a BMP file\n"
        "  --trace             print every executed instruction\n";
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "--activity") o.activity = next();
        else if (a == "--main") o.mainClass = next();
        else if (a == "--scale") o.scale = std::stof(next());
        else if (a == "--headless") o.headless = true;
        else if (a == "--run") o.runMs = std::stoll(next());
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--trace") o.trace = true;
        else if (a == "--tap") {
            std::string v = next();
            float x, y;
            long long at = 0;
            if (std::sscanf(v.c_str(), "%f,%f@%lld", &x, &y, &at) < 2) throw std::runtime_error("bad --tap " + v);
            o.taps.push_back({at, x, y});
        } else if (a == "--swipe") {
            std::string v = next();
            Options::Swipe sw{};
            long long at = 0;
            if (std::sscanf(v.c_str(), "%f,%f,%f,%f@%lld", &sw.x0, &sw.y0, &sw.x1, &sw.y1, &at) < 4)
                throw std::runtime_error("bad --swipe " + v);
            sw.at = at;
            o.swipes.push_back(sw);
        } else if (a == "--type") {
            std::string v = next();
            size_t at = v.rfind('@');
            o.typed.push_back({at == std::string::npos ? 0 : std::stoll(v.substr(at + 1)), v.substr(0, at)});
        } else if (a == "-h" || a == "--help") return false;
        else if (o.input.empty()) o.input = a;
        else throw std::runtime_error("unexpected argument " + a);
    }
    return !o.input.empty();
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Loads all classes*.dex of an APK (or a bare .dex) and finds the activity to start.
std::string loadProgram(VM& vm, const Options& o, std::string& label) {
    label = "droidvm";
    if (endsWith(o.input, ".dex")) {
        vm.loadDex(readFile(o.input));
        return o.activity;
    }
    ZipArchive apk(o.input);
    for (int n = 1;; ++n) {
        std::string name = n == 1 ? "classes.dex" : "classes" + std::to_string(n) + ".dex";
        auto dex = apk.read(name);
        if (!dex) {
            if (n == 1) throw std::runtime_error(o.input + " contains no classes.dex");
            break;
        }
        vm.loadDex(std::move(*dex));
    }
    std::string activity = o.activity;
    if (auto manifest = apk.read("AndroidManifest.xml")) {
        ManifestInfo info = readManifest(*manifest);
        if (!info.package.empty()) label = info.package.substr(info.package.rfind('.') + 1);
        if (activity.empty()) activity = info.launcherActivity;
    }
    if (activity.empty()) throw std::runtime_error("no launcher activity found; use --activity");
    return activity;
}

int runMain(VM& vm, const std::string& cls) {
    Class* c = vm.findClassOrNull(javaNameToDescriptor(cls));
    if (!c) { std::cerr << "class not found: " << cls << "\n"; return 1; }
    try {
        Reg args = fromObj(vm.newArray("[Ljava/lang/String;", 0));
        vm.callStatic(c, "main", "([Ljava/lang/String;)V", {args});
    } catch (JavaThrow& t) {
        std::cerr << "Exception in thread \"main\" " << vm.describeException(t.exception) << "\n";
        return 1;
    } catch (ExitRequest& e) {
        return e.code;
    }
    return 0;
}

bool saveBmp(const Raster& fb, const std::string& path) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint32_t*>(fb.pixels()), fb.width(), fb.height(),
                                                        32, fb.width() * 4, SDL_PIXELFORMAT_ARGB8888);
    bool ok = s && SDL_SaveBMP(s, path.c_str()) == 0;
    SDL_FreeSurface(s);
    return ok;
}

int runHeadless(AndroidRuntime& rt, int64_t& clock, const Options& o) {
    // Virtual time: 16 ms per frame, as fast as the CPU allows.
    for (; clock <= o.runMs && !rt.finished(); clock += 16) {
        for (auto& t : o.taps) {
            if (t.at >= clock - 16 && t.at < clock) {
                rt.touch(0, t.x, t.y);
                rt.touch(1, t.x, t.y);
            }
        }
        for (auto& sw : o.swipes) {
            // A swipe lasts 10 frames: down, 8 moves, up.
            int64_t step = (clock - sw.at) / 16;
            if (clock < sw.at || step > 9) continue;
            float t = float(step) / 9.f;
            float x = sw.x0 + (sw.x1 - sw.x0) * t, y = sw.y0 + (sw.y1 - sw.y0) * t;
            rt.touch(step == 0 ? 0 : step == 9 ? 1 : 2, x, y);
        }
        for (auto& [at, text] : o.typed)
            if (at >= clock - 16 && at < clock) rt.textInput(utf8ToUtf16(text));
        rt.tick();
        if (rt.needsRedraw()) rt.render();
    }
    if (!rt.finished()) rt.render();
    if (!o.screenshot.empty() && !saveBmp(rt.framebuffer(), o.screenshot)) {
        std::cerr << "cannot write " << o.screenshot << ": " << SDL_GetError() << "\n";
        return 1;
    }
    int code = rt.exitCode();
    rt.shutdown();
    return code;
}

int runWindow(AndroidRuntime& rt, const Options& o, const std::string& label) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init: " << SDL_GetError() << "\n";
        return 1;
    }
    const Screen& s = rt.screen();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_Window* win = SDL_CreateWindow(("droidvm - " + label).c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       int(s.width * o.scale), int(s.height * o.scale), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren) {
        std::cerr << "SDL: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_RenderSetLogicalSize(ren, s.width, s.height);  // also maps mouse coordinates for us
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, s.width, s.height);
    SDL_StartTextInput();

    bool mouseDown = false;
    while (!rt.finished()) {
        SDL_Event e;
        int64_t wait = rt.msUntilNextTimer();
        int timeout = rt.needsRedraw() ? 0 : int(wait < 0 ? 500 : std::min<int64_t>(wait, 500));
        if (SDL_WaitEventTimeout(&e, timeout)) {
            do {
                switch (e.type) {
                case SDL_QUIT: rt.shutdown(); break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.button == SDL_BUTTON_LEFT) { mouseDown = true; rt.touch(0, float(e.button.x), float(e.button.y)); }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (e.button.button == SDL_BUTTON_LEFT && mouseDown) { mouseDown = false; rt.touch(1, float(e.button.x), float(e.button.y)); }
                    break;
                case SDL_MOUSEMOTION:
                    if (mouseDown) rt.touch(2, float(e.motion.x), float(e.motion.y));
                    break;
                case SDL_MOUSEWHEEL: {
                    int mx, my;
                    SDL_GetMouseState(&mx, &my);
                    float lx, ly;
                    SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly);
                    rt.scroll(lx, ly, float(-e.wheel.y) * 60 * s.density);
                    break;
                }
                case SDL_TEXTINPUT: rt.textInput(utf8ToUtf16(e.text.text)); break;
                case SDL_KEYDOWN:
                    if (e.key.keysym.sym == SDLK_BACKSPACE) rt.backspace();
                    else if (e.key.keysym.sym == SDLK_ESCAPE) rt.back();
                    break;
                case SDL_WINDOWEVENT: rt.invalidate(); break;
                }
            } while (!rt.finished() && SDL_PollEvent(&e));
        }
        if (rt.finished()) break;
        rt.tick();
        if (rt.needsRedraw()) {
            rt.render();
            SDL_UpdateTexture(tex, nullptr, rt.framebuffer().pixels(), s.width * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);
        }
    }
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return rt.exitCode();
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        if (!parseArgs(argc, argv, o)) { usage(); return 2; }
    } catch (std::exception& e) {
        std::cerr << e.what() << "\n";
        usage();
        return 2;
    }
    try {
        VM vm;
        vm.trace = o.trace;
        std::string label;
        std::string activity = loadProgram(vm, o, label);
        if (!o.mainClass.empty()) return runMain(vm, o.mainClass);
        if (activity.empty()) { std::cerr << "nothing to run: give --main or --activity\n"; return 2; }

        int64_t virtualClock = 0;
        auto start = std::chrono::steady_clock::now();
        std::function<int64_t()> clock = o.headless
            ? std::function<int64_t()>([&] { return virtualClock; })
            : std::function<int64_t()>([start] {
                  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
              });
        AndroidRuntime rt(vm, Screen{}, clock);
        rt.launch(activity, label);
        return o.headless ? runHeadless(rt, virtualClock, o) : runWindow(rt, o, label);
    } catch (std::exception& e) {
        std::cerr << "droidvm: " << e.what() << "\n";
        return 1;
    }
}
