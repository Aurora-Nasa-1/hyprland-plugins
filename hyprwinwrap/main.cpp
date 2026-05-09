#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <vector>

#include <hyprland/src/includes.hpp>
#include <any>
#include <sstream>

#define private public
#define protected public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/rule/Engine.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#undef private
#undef protected

#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include "globals.hpp"

// Do NOT change this function
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// hooks
inline CFunctionHook* subsurfaceHook = nullptr;
inline CFunctionHook* commitHook     = nullptr;
typedef void (*origCommitSubsurface)(Desktop::View::CSubsurface* thisptr);
typedef void (*origCommit)(void* owner, void* data);

struct OriginalWindowState {
    bool isFloating;
    bool isPinned;
    Vector2D size;
    Vector2D position;
};

struct BgWindowData {
    PHLWINDOWREF window;
    OriginalWindowState originalState;
};

std::vector<BgWindowData> bgWindows;
std::vector<SP<Desktop::Rule::IRule>> bgRules;

static SP<Desktop::Rule::CWindowRule> makeWindowRule(const std::string& name, const Desktop::Rule::eRuleProperty prop, const std::string& match) {
    auto rule = makeShared<Desktop::Rule::CWindowRule>(name);
    rule->registerMatch(prop, "^(" + match + ")$");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_FLOAT, "1");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_SIZE, "100% 100%");
    return rule;
}

static void clearWindowRules() {
    for (auto& rule : bgRules) {
        if (rule)
            Desktop::Rule::ruleEngine()->unregisterRule(rule);
    }
    bgRules.clear();
}

void unwrapWindow(PHLWINDOW pWindow);

void wrapWindow(PHLWINDOW pWindow) {
    if (std::find_if(bgWindows.begin(), bgWindows.end(), [pWindow](const auto& data) { return data.window.lock() == pWindow; }) != bgWindows.end())
        return; // already wrapped

    // Enforce max 1 background window
    while (!bgWindows.empty()) {
        auto oldWin = bgWindows.front().window.lock();
        if (oldWin) {
            unwrapWindow(oldWin);
        } else {
            bgWindows.erase(bgWindows.begin());
        }
    }

    const auto PMONITOR = pWindow->m_monitor.lock();
    if (!PMONITOR)
        return;

    OriginalWindowState originalState;
    originalState.isFloating = pWindow->m_isFloating;
    originalState.isPinned = pWindow->m_pinned;
    originalState.size = pWindow->m_size;
    originalState.position = pWindow->m_position;

    if (!pWindow->m_isFloating)
        g_layoutManager->changeFloatingMode(pWindow->layoutTarget());

    float sx = 100.f, sy = 100.f, px = 0.f, py = 0.f;

    static auto* const PSIZEX = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:size_x")->getDataStaticPtr();
    static auto* const PSIZEY = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:size_y")->getDataStaticPtr();
    static auto* const PPOSX  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_x")->getDataStaticPtr();
    static auto* const PPOSY  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_y")->getDataStaticPtr();

    try {
        sx = std::stof(*PSIZEX);
    } catch (...) {}
    try {
        sy = std::stof(*PSIZEY);
    } catch (...) {}
    try {
        px = std::stof(*PPOSX);
    } catch (...) {}
    try {
        py = std::stof(*PPOSY);
    } catch (...) {}

    sx = std::clamp(sx, 1.f, 100.f);
    sy = std::clamp(sy, 1.f, 100.f);
    px = std::clamp(px, 0.f, 100.f);
    py = std::clamp(py, 0.f, 100.f);

    if (px + sx > 100.f) {
        Log::logger->log(Log::WARN, "[hyprwinwrap] size_x (%f) + pos_x (%f) > 100, adjusting size_x to %f", sx, px, 100.f - px);
        sx = 100.f - px;
    }
    if (py + sy > 100.f) {
        Log::logger->log(Log::WARN, "[hyprwinwrap] size_y (%f) + pos_y (%f) > 100, adjusting size_y to %f", sy, py, 100.f - py);
        sy = 100.f - py;
    }

    const Vector2D monitorSize = PMONITOR->m_size;
    const Vector2D monitorPos  = PMONITOR->m_position;

    const Vector2D newSize = {static_cast<double>(monitorSize.x * (sx / 100.f)), static_cast<double>(monitorSize.y * (sy / 100.f))};
    const Vector2D newPos = {static_cast<double>(monitorPos.x + (monitorSize.x * (px / 100.f))), static_cast<double>(monitorPos.y + (monitorSize.y * (py / 100.f)))};

    const CBox b(newPos.x, newPos.y, newSize.x, newSize.y);

    pWindow->layoutTarget()->space()->setTargetGeom(b, pWindow->layoutTarget());
    pWindow->m_realSize->setValueAndWarp(newSize);
    pWindow->m_realPosition->setValueAndWarp(newPos);
    pWindow->m_size     = newSize;
    pWindow->m_position = newPos;
    pWindow->m_pinned   = true;
    pWindow->updateWindowData();
    pWindow->sendWindowSize(true);

    bgWindows.push_back({pWindow, originalState});
    pWindow->m_hidden = true;

    g_pInputManager->refocus();
    Log::logger->log(Log::DEBUG, "[hyprwinwrap] window {} moved to bg", pWindow);
}

void unwrapWindow(PHLWINDOW pWindow) {
    auto it = std::find_if(bgWindows.begin(), bgWindows.end(), [pWindow](const auto& data) { return data.window.lock() == pWindow; });
    if (it == bgWindows.end())
        return; // not wrapped

    OriginalWindowState originalState = it->originalState;
    bgWindows.erase(it);

    pWindow->m_hidden = false;
    pWindow->m_pinned = originalState.isPinned;

    if (pWindow->m_isFloating != originalState.isFloating)
        g_layoutManager->changeFloatingMode(pWindow->layoutTarget());

    if (originalState.isFloating) {
        pWindow->m_realSize->setValueAndWarp(originalState.size);
        pWindow->m_realPosition->setValueAndWarp(originalState.position);
        pWindow->m_size     = originalState.size;
        pWindow->m_position = originalState.position;
        pWindow->updateWindowData();
        pWindow->sendWindowSize(true);
    }

    g_pInputManager->refocus();
    Log::logger->log(Log::DEBUG, "[hyprwinwrap] window {} restored from bg", pWindow);
}

void toggleWindow(std::string args) {
    if (!bgWindows.empty()) {
        auto oldWin = bgWindows.front().window.lock();
        if (oldWin) {
            unwrapWindow(oldWin);
        } else {
            bgWindows.erase(bgWindows.begin());
        }
        return;
    }

    PHLWINDOW pWindow = nullptr;
    for (auto& w : g_pCompositor->m_windows) {
        if (g_pCompositor->isWindowActive(w)) {
            pWindow = w;
            break;
        }
    }
    
    if (pWindow) {
        wrapWindow(pWindow);
    }
}

void onNewWindow(PHLWINDOW pWindow) {
    static auto* const PCLASS = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:class")->getDataStaticPtr();
    static auto* const PTITLE = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:title")->getDataStaticPtr();

    const std::string  classRule(*PCLASS);
    const std::string  titleRule(*PTITLE);

    const bool         classMatches = !classRule.empty() && pWindow->m_initialClass == classRule;
    const bool         titleMatches = !titleRule.empty() && pWindow->m_title == titleRule;

    if (!classMatches && !titleMatches)
        return;

    wrapWindow(pWindow);
}

void onCloseWindow(PHLWINDOW pWindow) {
    std::erase_if(bgWindows, [pWindow](const auto& data) { return data.window.expired() || data.window.lock() == pWindow; });

    Log::logger->log(Log::DEBUG, "[hyprwinwrap] closed window {}", pWindow);
}

void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WALLPAPER)
        return;

    for (auto& bg : bgWindows) {
        const auto bgw = bg.window.lock();
        if (!bgw) continue;

        if (bgw->m_monitor != g_pHyprOpenGL->m_renderData.pMonitor)
            continue;

        // cant use setHidden cuz that sends suspended and shit too that would be laggy
        bgw->m_hidden = false;

        g_pHyprRenderer->renderWindow(bgw, g_pHyprOpenGL->m_renderData.pMonitor.lock(), Time::steadyNow(), false, RENDER_PASS_ALL, false, true);

        bgw->m_hidden = true;
    }
}

void onCommitSubsurface(Desktop::View::CSubsurface* thisptr) {
    const auto PWINDOW = Desktop::View::CWindow::fromView(thisptr->wlSurface()->view());

    if (!PWINDOW || std::find_if(bgWindows.begin(), bgWindows.end(), [PWINDOW](const auto& data) { return data.window.lock() == PWINDOW; }) == bgWindows.end()) {
        ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
        return;
    }

    // cant use setHidden cuz that sends suspended and shit too that would be laggy
    PWINDOW->m_hidden = false;

    ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprRenderer->damageMonitor(MON);

    PWINDOW->m_hidden = true;
}

void onCommit(void* owner, void* data) {
    const auto PWINDOW = ((Desktop::View::CWindow*)owner)->m_self.lock();

    if (std::find_if(bgWindows.begin(), bgWindows.end(), [PWINDOW](const auto& data) { return data.window.lock() == PWINDOW; }) == bgWindows.end()) {
        ((origCommit)commitHook->m_original)(owner, data);
        return;
    }

    // cant use setHidden cuz that sends suspended and shit too that would be laggy
    PWINDOW->m_hidden = false;

    ((origCommit)commitHook->m_original)(owner, data);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprRenderer->damageMonitor(MON);

    PWINDOW->m_hidden = true;
}

void onConfigReloaded() {
    clearWindowRules();

    static auto* const PCLASS = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:class")->getDataStaticPtr();
    const std::string  classRule(*PCLASS);
    if (!classRule.empty()) {
        auto rule = makeWindowRule("hyprwinwrap-class", Desktop::Rule::RULE_PROP_CLASS, classRule);
        bgRules.emplace_back(rule);
        Desktop::Rule::ruleEngine()->registerRule(SP<Desktop::Rule::IRule>{rule});
    }

    static auto* const PTITLE = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:title")->getDataStaticPtr();
    const std::string  titleRule(*PTITLE);
    if (!titleRule.empty()) {
        auto rule = makeWindowRule("hyprwinwrap-title", Desktop::Rule::RULE_PROP_TITLE, titleRule);
        bgRules.emplace_back(rule);
        Desktop::Rule::ruleEngine()->registerRule(SP<Desktop::Rule::IRule>{rule});
    }

    Desktop::Rule::ruleEngine()->updateAllRules();
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hww] Version mismatch");
    }

    static auto P  = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onNewWindow(w); });
    static auto P2 = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { onCloseWindow(w); });
    static auto P3 = Event::bus()->m_events.render.stage.listen([&](eRenderStage stage) { onRenderStage(stage); });
    static auto P4 = Event::bus()->m_events.config.reloaded.listen([&] { onConfigReloaded(); });

    auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View11CSubsurface8onCommitEv");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: onCommit not found");
    subsurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommitSubsurface);

    fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View7CWindow12commitWindowEv");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: listener_commitWindow not found");
    commitHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommit);

    bool hkResult = subsurfaceHook->hook();
    hkResult      = hkResult && commitHook->hook();

    if (!hkResult)
        throw std::runtime_error("hyprwinwrap: hooks failed");

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:class", Hyprlang::STRING{"kitty-bg"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:title", Hyprlang::STRING{""});

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:size_x", Hyprlang::STRING{"100"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:size_y", Hyprlang::STRING{"100"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_x", Hyprlang::STRING{"0"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_y", Hyprlang::STRING{"0"});

    HyprlandAPI::addDispatcher(PHANDLE, "hyprwinwrap:toggle", toggleWindow);

    onConfigReloaded();

    HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwinwrap", "A clone of xwinwrap for Hyprland", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    clearWindowRules();
}
