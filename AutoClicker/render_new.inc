// ============================================================
//  glass render: 三层渲染 (chrome / chromeDyn / content) + 合成呈现
// ============================================================
// 由 main.cpp 包含 (render_new.inc)。渲染顺序: 基底玻璃在 chrome 层,
// 状态栏动态内容在 chromeDyn 层, 页面内容在 content 层; Present 按
// chrome -> chromeDyn -> content(动画偏移+淡入) 依次混合后 ULW 呈现。

// ---- 悬停提示气泡 ----
static void RenderTip(GLayer& l, const RECT& anchor, const wchar_t* const* lines, int n)
{
    HDC dc = l.dc;
    SelectObject(dc, g_hfSmall);
    int w = 0;
    for (int i = 0; i < n; i++) {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, lines[i], (int)wcslen(lines[i]), &sz);
        if (sz.cx > w) w = sz.cx;
    }
    int padX = 10, padY = 6, lineH = 17;
    int tw = w + padX * 2;
    int th = n * lineH + padY * 2 - 4;
    RECT t = { anchor.left, anchor.bottom + 5,
               anchor.left + tw, anchor.bottom + 5 + th };
    if (t.bottom > g_cy - 8) {
        t.top = anchor.top - 5 - th;
        t.bottom = anchor.top - 5;
    }
    if (t.right > g_cx - 8) { int dx = t.right - (g_cx - 8); t.left -= dx; t.right -= dx; }
    if (t.left < 8)         { int dx = 8 - t.left;         t.left += dx; t.right += dx; }
    if (t.top < 8)          t.top = 8;
    // 气泡用更实的玻璃底, 保证可读性
    GPanel(l, t, 8, PANEL(), (BYTE)(g_theme == Theme::Dark ? 168 : 200));
    RECT tr = { t.left + padX, t.top + padY - 3, t.right - padX, t.bottom - padY };
    for (int i = 0; i < n; i++) {
        RECT lr = tr;
        lr.top += i * lineH;
        lr.bottom = lr.top + lineH;
        GText(l, g_hfSmall, TXT_DIM(), lines[i], lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// ============================================================
//  chrome 层: 基底玻璃 + 标题 + 芯片 + 侧栏 + 状态栏底板
// ============================================================
static void RenderChrome()
{
    if (!g_layersOk) return;
    GLayer& l = g_chrome;
    GLClear(l);
    HDC dc = l.dc;

    // ---- 基底玻璃: 垂直渐变 + 顶部氛围高光 (底角圆角, 桌面透出) ----
    {
        BYTE aTop = (BYTE)(g_theme == Theme::Dark ? 214 : 222);
        BYTE aBot = (BYTE)(g_theme == Theme::Dark ? 202 : 212);
        GLFillV(l, { 0, 0, g_cx, g_cy }, 12, BGTOP(), aTop, BGBOT(), aBot);
        GLFillV(l, { 0, 0, g_cx, g_cy / 3 }, 12, SHEEN(),
                (BYTE)(g_theme == Theme::Dark ? 7 : 30), SHEEN(), 0);
    }

    // ---- 标题 ----
    {
        RECT tr = L.title;
        tr.left += 10; tr.top += 1; tr.right = L.btnPin.left - 14;
        GText(l, g_hfTitle, TXT(), L"AutoClicker", tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT vr = { tr.left, tr.top + 25, tr.right, tr.bottom };
        std::wstring verStr = std::wstring(L"v") + APP_VERSION_W;
        GText(l, g_hfSmall, TXT_DIM(), verStr.c_str(), vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- 图钉 / 主题按钮 ----
    {
        RECT& b = L.btnPin;
        bool hover = g_hr[E_BTN_PIN].hover;
        GButton(l, b, 13, hover, false, topmost);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        DrawPin(dc, cx, cy, topmost);
        GLLiftAlphaRect(l, b);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(),
                  topmost ? L"取消置顶" : L"置顶窗口",
                  tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    {
        RECT& b = L.btnTheme;
        bool hover = g_hr[E_BTN_THEME].hover;
        GButton(l, b, 13, hover, false, false);
        GText(l, g_hfBody, TXT(), g_theme == Theme::Dark ? L"\x2600" : L"\x263E",
              b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(),
                  g_theme == Theme::Dark ? L"切换亮色" : L"切换深色",
                  tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---- 方案芯片 (标题栏) ----
    for (int i = 0; i < 4; i++) {
        RECT& b = L.prfChip[i];
        bool sel = ((i + 1) == g_activeProfile);
        bool hover = g_hr[E_PRF0 + i].hover;
        GButton(l, b, 12, hover, false, sel);
        RECT tr = { b.left + 4, b.top, b.right - 4, b.bottom };
        GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT()),
              g_profileNames[i].c_str(), tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // ---- 侧边栏 ----
    GPanel(l, L.sidebar, 14, PANEL(), (BYTE)(g_theme == Theme::Dark ? 62 : 104));
    static const wchar_t* navNames[5] = { L"连点", L"多倍", L"滚轮", L"面板", L"高级" };
    for (int i = 0; i < 5; i++) {
        RECT& b = L.nav[i];
        bool sel = ((int)g_page == i);
        bool hover = g_hr[E_NAV_CLICK + i].hover;
        GButton(l, b, 10, hover, false, sel);
        int cx = (b.left + b.right) / 2;
        int icY = b.top + 13;
        COLORREF ic = sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT_DIM());
        switch (i) {
        case 0: DrawMouseIcon(dc, cx, icY, ic); break;
        case 1: DrawMultiIcon(dc, cx, icY, ic); break;
        case 2: DrawWheelIcon(dc, cx, icY, ic); break;
        case 3: DrawDashIcon(dc, cx, icY, ic); break;
        case 4: DrawAdvIcon(dc, cx, icY, ic); break;
        }
        GLLiftAlphaRect(l, b);
        RECT lr = { b.left, b.top + 32, b.right, b.bottom - 3 };
        GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT_DIM()),
              navNames[i], lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- 状态栏底板 ----
    GPanel(l, L.status, 12, PANEL(), (BYTE)(g_theme == Theme::Dark ? 62 : 104));
}

// ============================================================
//  chromeDyn 层: 状态栏动态内容 (10Hz 级刷新, 整层很小)
// ============================================================
static void RenderChromeDyn()
{
    if (!g_layersOk) return;
    GLayer& l = g_chromeDyn;
    GLClear(l);
    HDC dc = l.dc;
    int baseY = L.status.top + 10;

    // 槽1 连点
    {
        RECT& s = L.stSlot[0];
        COLORREF clr = isstart ? GREEN() : RED();
        GDot(l, s.left + 4, baseY, clr, isstart);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, isstart ? L"连点 开" : L"连点 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽2 辅助 (多倍 + 滚轮)
    {
        RECT& s = L.stSlot[1];
        bool on = isMultiActive || isScrollClickActive;
        COLORREF clr = on ? ACCENT() : RED();
        GDot(l, s.left + 4, baseY, clr, on);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, on ? L"辅助 开" : L"辅助 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽3 攻击门控: dot = 实时可攻击态, 文字 = 功能开关
    {
        RECT& s = L.stSlot[2];
        COLORREF clr = RED();
        if (!canAttackOnlyClick) clr = RED();
        else if (!CanAttackConnected()) clr = TXT_DIM();
        else clr = g_canAttack.load(std::memory_order_relaxed) ? GREEN() : RED();
        GDot(l, s.left + 4, baseY, clr, canAttackOnlyClick && g_canAttack.load() == 1);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, canAttackOnlyClick ? L"攻击 开" : L"攻击 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽4 放置门控
    {
        RECT& s = L.stSlot[3];
        COLORREF clr = RED();
        if (!placeOnlyRightClick) clr = RED();
        else if (!CanAttackConnected()) clr = TXT_DIM();
        else clr = g_canPlace.load(std::memory_order_relaxed) ? GREEN() : RED();
        GDot(l, s.left + 4, baseY, clr, placeOnlyRightClick && g_canPlace.load() == 1);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, placeOnlyRightClick ? L"放置 开" : L"放置 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽5 目标: 准星目标类名 (短名)
    {
        RECT& s = L.stSlot[4];
        bool conn = CanAttackConnected();
        char tname[128] = {};
        GetShmTargetName(tname, sizeof(tname));
        COLORREF clr = TXT_DIM();
        std::wstring txt = L"目标 —";
        if (conn && tname[0]) {
            clr = ACCENT();
            const char* last = tname;
            for (const char* p = tname; *p; p++)
                if (*p == '.' || *p == '/' || *p == '$') last = p + 1;
            wchar_t wnm[128];
            MultiByteToWideChar(CP_UTF8, 0, last, -1, wnm, 128);
            txt = L"目标 " + std::wstring(wnm);
        }
        GDot(l, s.left + 4, baseY, clr, conn && tname[0]);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, txt.c_str(), r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // 槽6 CPS: 迷你曲线 + 数值
    {
        RECT& s = L.stSlot[5];
        GChip(l, s, 8);
        int sw = (s.right - s.left) - 54;
        if (sw > 76) sw = 76;
        if (sw < 20) sw = 20;
        int n = s_sparkCount < kSparkN ? s_sparkCount : kSparkN;
        int need = sw / 2;
        if (need > n) need = n;
        if (need >= 2) {
            int maxV = 1;
            for (int i = 0; i < need; i++) {
                int v = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                if (v > maxV) maxV = v;
            }
            if (maxV < 10) maxV = 10;
            POINT pts[64];
            for (int i = 0; i < need; i++) {
                int v = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                pts[i] = { s.left + 4 + sw - i * 2,
                           s.bottom - 4 - (s.bottom - s.top - 10) * v / maxV };
            }
            HPEN sp = CreatePen(PS_SOLID, 1, ACCENT());
            HGDIOBJ op = SelectObject(dc, sp);
            Polyline(dc, pts, need);
            SelectObject(dc, op);
            DeleteObject(sp);
            GLLiftAlphaRect(l, s);
        }
        wchar_t cb[32];
        swprintf(cb, 32, L"%d/s", GetRealtimeCps());
        RECT tr = { s.left + sw + 10, s.top, s.right - 4, s.bottom };
        GText(l, g_hfSmall, TXT(), cb, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// ============================================================
//  content 层: 当前页全部内容 (页切换动画只移动这一层)
// ============================================================
static void RenderContent()
{
    if (!g_layersOk) return;
    GLayer& l = g_content;
    GLClear(l);
    HDC dc = l.dc;

    // ---- 卡片 ----
    int cardCount = 0;
    switch (g_page) {
    case PAGE_CLICK: cardCount = 3; break;
    case PAGE_MULTI: cardCount = 3; break;
    case PAGE_SCROLL: cardCount = 1; break;
    case PAGE_DASH: cardCount = 2; break;
    case PAGE_ADV: cardCount = 3; break;
    default: break;
    }
    for (int i = 0; i < cardCount; i++)
        GPanel(l, L.card[i], 14, CARD(), (BYTE)(g_theme == Theme::Dark ? 74 : 116));

    // ---- 卡片标签 ----
    static const wchar_t* namesClick[3] = {
        L"左键", L"右键", L"快捷键 与 保持"
    };
    static const wchar_t* namesMulti[3] = {
        L"倍率", L"延迟", L"快捷键"
    };
    static const wchar_t* namesScroll[1] = { L"滚轮点击" };
    static const wchar_t* namesDash[2] = {
        L"实时 CPS 面板", L"游戏状态 与 个性化"
    };
    static const wchar_t* namesAdv[3] = {
        L"CPS 上限", L"随机 CPS", L"定时停止 与 拟人化"
    };
    const wchar_t** names = namesClick;
    switch (g_page) {
    case PAGE_CLICK: names = namesClick; break;
    case PAGE_MULTI: names = namesMulti; break;
    case PAGE_SCROLL: names = namesScroll; break;
    case PAGE_DASH: names = namesDash; break;
    case PAGE_ADV: names = namesAdv; break;
    }
    for (int i = 0; i < cardCount; i++) {
        RECT r = { L.card[i].left + 20, L.card[i].top + S(8),
                   L.card[i].right, L.card[i].top + S(26) };
        GText(l, g_hfLabel, TXT(), names[i], r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    auto DrawKeyBtn = [&](const RECT& b, const std::wstring& label,
                          Elem elem, bool rebinding) {
        GButton(l, b, 10, g_hr[elem].hover, false, rebinding);
        GText(l, g_hfBody, rebinding ? RGB(255, 255, 255) : TXT(),
              label.c_str(), b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    auto DrawPresetRow = [&](const RECT* pre, int baseElem,
                             const wchar_t* const* labels, int selIdx) {
        for (int k = 0; k < 4; k++) {
            const RECT& b = pre[k];
            bool sel = (selIdx == k);
            bool hover = g_hr[baseElem + k].hover;
            GButton(l, b, 8, hover, false, sel);
            GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT()),
                  labels[k], b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    };

    // ================= PAGE: CLICK =================
    if (g_page == PAGE_CLICK) {
        GToggle(l, L.tglL, leftenabled);
        GToggle(l, L.tglR, rightenabled);
        GSlider(l, SL_L, E_SL_L);
        GSlider(l, SL_R, E_SL_R);

        static const wchar_t* cpsLbl[4] = { L"6/s", L"10/s", L"15/s", L"20/s" };
        int selL = -1, selR = -1;
        for (int k = 0; k < 4; k++) {
            if (cpsLeft10  == kCpsPresets[k] * 10) selL = k;
            if (cpsRight10 == kCpsPresets[k] * 10) selR = k;
        }
        DrawPresetRow(L.preL, E_PRE_L0, cpsLbl, selL);
        DrawPresetRow(L.preR, E_PRE_R0, cpsLbl, selR);

        // 数值
        for (int i = 0; i < 2; i++) {
            int c10 = (i == 0) ? cpsLeft10 : cpsRight10;
            int ms  = (i == 0) ? leftms : rightms;
            wchar_t buf[32];
            swprintf(buf, 32, L"%.1f 次/秒", c10 / 10.0f);
            RECT r = { L.card[i].left + 20, L.card[i].top + 60,
                       L.track[SL_L + i].right, L.card[i].top + 86 };
            GText(l, g_hfLabel, TXT(), buf, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            swprintf(buf, 32, L"%d 毫秒", ms);
            RECT r2 = { r.right + 8, r.top, L.card[i].right - 8, r.bottom };
            GText(l, g_hfSmall, TXT_DIM(), buf, r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // 快捷键
        {
            RECT& b = L.btnKey;
            std::wstring t = (g_rebinding == E_BTN_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_key);
            DrawKeyBtn(b, t, E_BTN_KEY, g_rebinding == E_BTN_KEY);
        }
        // 保持模式
        {
            RECT& b = L.btnKeep;
            bool hover = g_hr[E_BTN_KEEP].hover;
            GButton(l, b, 10, hover, false, keepClicke);
            GText(l, g_hfBody, keepClicke ? RGB(255, 255, 255) : TXT(),
                  keepClicke ? L"不需要按住连点: 开" : L"不需要按住连点: 关",
                  b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // 仅能攻击时连点
        {
            RECT& b = L.btnCanAtk;
            bool hover = g_hr[E_BTN_CANATK].hover;
            GButton(l, b, 10, hover, false, canAttackOnlyClick);
            GText(l, g_hfBody, canAttackOnlyClick ? RGB(255, 255, 255) : TXT(),
                  canAttackOnlyClick ? L"仅能攻击时连点: 开" : L"仅能攻击时连点: 关",
                  b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& chip = L.canAtkChip;
            GChip(l, chip, 8);
            COLORREF cc = TXT_DIM();
            const wchar_t* txt = L"未连接";
            if (CanAttackConnected()) {
                if (g_canAttack.load(std::memory_order_relaxed) == 1) {
                    cc = GREEN(); txt = L"可攻击";
                } else {
                    cc = RED(); txt = L"不可攻击";
                }
            }
            GText(l, g_hfSmall, cc, txt, chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& b = L.btnCanAtkKey;
            std::wstring t = (g_rebinding == E_BTN_CANATK_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_canattack_key);
            DrawKeyBtn(b, t, E_BTN_CANATK_KEY, g_rebinding == E_BTN_CANATK_KEY);
        }
        // 仅手持放置物时右键连点
        {
            RECT& b = L.btnPlace;
            bool hover = g_hr[E_BTN_PLACE].hover;
            GButton(l, b, 10, hover, false, placeOnlyRightClick);
            GText(l, g_hfBody, placeOnlyRightClick ? RGB(255, 255, 255) : TXT(),
                  placeOnlyRightClick ? L"仅手持放置物时右键连点: 开" : L"仅手持放置物时右键连点: 关",
                  b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& chip = L.placeChip;
            GChip(l, chip, 8);
            COLORREF cc = TXT_DIM();
            const wchar_t* txt = L"未连接";
            if (CanAttackConnected()) {
                if (g_canPlace.load(std::memory_order_relaxed) == 1) {
                    cc = GREEN(); txt = L"手持放置物";
                } else {
                    cc = RED(); txt = L"非放置物";
                }
            }
            GText(l, g_hfSmall, cc, txt, chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& b = L.btnPlaceKey;
            std::wstring t = (g_rebinding == E_BTN_PLACE_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_place_key);
            DrawKeyBtn(b, t, E_BTN_PLACE_KEY, g_rebinding == E_BTN_PLACE_KEY);
        }
    }

    // ================= PAGE: MULTI =================
    if (g_page == PAGE_MULTI) {
        GSlider(l, SL_MUL, E_SL_MUL);
        GSlider(l, SL_DEL, E_SL_DEL);

        static const wchar_t* mulLbl[4] = { L"2x", L"3x", L"4x", L"5x" };
        static const wchar_t* delLbl[4] = { L"10", L"25", L"50", L"100" };
        int selM = -1, selD = -1;
        for (int k = 0; k < 4; k++) {
            if (multiMul == kMulPresets[k])       selM = k;
            if (multiDelayMs == kDelayPresets[k]) selD = k;
        }
        DrawPresetRow(L.preM, E_PRE_M0, mulLbl, selM);
        DrawPresetRow(L.preD, E_PRE_D0, delLbl, selD);

        wchar_t buf[32];
        swprintf(buf, 32, L"%d 倍", multiMul);
        RECT r0 = { L.card[0].left + 20, L.card[0].top + 60,
                    L.track[SL_MUL].right, L.card[0].top + 86 };
        GText(l, g_hfLabel, TXT(), buf, r0, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        swprintf(buf, 32, L"%d 毫秒", multiDelayMs);
        RECT r1 = { L.card[1].left + 20, L.card[1].top + 60,
                    L.track[SL_DEL].right, L.card[1].top + 86 };
        GText(l, g_hfLabel, TXT(), buf, r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        {
            RECT& b = L.btnMKey;
            std::wstring t = (g_rebinding == E_BTN_MKEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_multi_key);
            DrawKeyBtn(b, t, E_BTN_MKEY, g_rebinding == E_BTN_MKEY);
        }
        RECT hint = { L.card[2].left + 20, L.card[2].top + S(68),
                      L.card[2].right - 20, L.card[2].top + S(90) };
        GText(l, g_hfSmall, TXT_DIM(), L"按 + / - 键可快速微调倍数",
              hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ================= PAGE: SCROLL =================
    if (g_page == PAGE_SCROLL) {
        GToggle(l, L.tglScroll, isScrollClickActive);

        {
            std::wstring t = (g_rebinding == E_BTN_SCROLL_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_scroll_key);
            DrawKeyBtn(L.btnScrollKey, t, E_BTN_SCROLL_KEY,
                       g_rebinding == E_BTN_SCROLL_KEY);
        }
        {
            std::wstring t = (g_rebinding == E_BTN_SCROLL_LR_KEY)
                ? L"请按下新键…"
                : L"切换 L/R: " + getKeyName(vk_scroll_lr_key);
            DrawKeyBtn(L.btnScrollLRKey, t, E_BTN_SCROLL_LR_KEY,
                       g_rebinding == E_BTN_SCROLL_LR_KEY);
        }
        // 左/右分段选择器 (胶囊式)
        {
            RECT& b = L.btnScrollLR;
            int midX = (b.left + b.right) / 2;
            GButton(l, b, 8, false, false, false);
            RECT half = { scrollClickButton == 0 ? b.left : midX, b.top,
                          scrollClickButton == 0 ? midX : b.right, b.bottom };
            GLFillRound(l, half, 8, ACCENT(), 216);
            GText(l, g_hfBody, scrollClickButton == 0 ? RGB(255, 255, 255) : TXT_DIM(),
                  L"左键", { b.left, b.top, midX, b.bottom },
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            GText(l, g_hfBody, scrollClickButton == 1 ? RGB(255, 255, 255) : TXT_DIM(),
                  L"右键", { midX, b.top, b.right, b.bottom },
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RECT hint = { L.card[0].left + 20, L.card[0].top + S(110),
                      L.card[0].right - 20, L.card[0].top + S(132) };
        GText(l, g_hfSmall, TXT_DIM(), L"滚动滚轮时触发点击，向上/向下均可",
              hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ================= PAGE: DASH =================
    if (g_page == PAGE_DASH) {
        // 实时 CPS 大字
        {
            wchar_t buf[48];
            swprintf(buf, 48, L"%d 次/秒", GetRealtimeCps());
            GText(l, g_hfTitle, ACCENT(), buf, L.dashCps,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        // CPS 曲线
        {
            RECT& pr = L.dashPlot;
            GInset(l, pr, 10, false);
            int pw = pr.right - pr.left;
            int ph = pr.bottom - pr.top;
            for (int i = 1; i <= 3; i++) {
                int gy = pr.top + ph * i / 4;
                GLHLine(l, pr.left + 6, pr.right - 6, gy, TRACK(),
                        (BYTE)(g_theme == Theme::Dark ? 14 : 26));
            }
            int n = s_sparkCount < kSparkN ? s_sparkCount : kSparkN;
            if (n >= 2) {
                int maxV = 1;
                for (int i = 0; i < n; i++)
                    if (s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN] > maxV)
                        maxV = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                if (maxV < 10) maxV = 10;
                POINT pts[kSparkN + 2];
                int xStep = (pw - 24) / (kSparkN - 1);
                for (int i = 0; i < n; i++) {
                    int v = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                    int x = pr.right - 12 - i * xStep;
                    int y = pr.bottom - 8 - (pr.bottom - pr.top - 20) * v / maxV;
                    pts[i] = { x, y };
                }
                // 面积 (软件列填充, 半透明)
                for (int i = 0; i < n; i++) {
                    int x = pts[i].x;
                    int y = pts[i].y;
                    if (y < pr.bottom - 7)
                        GLFillRound(l, { x, y, x + 2, pr.bottom - 7 }, 1, ACCENT(), 44);
                }
                // 折线 (GDI, 不透明)
                HPEN alp = CreatePen(PS_SOLID, 1, ACCENT());
                HGDIOBJ op = SelectObject(dc, alp);
                Polyline(dc, pts, n);
                SelectObject(dc, op);
                DeleteObject(alp);
                GLLiftAlphaRect(l, pr);
                // 最新点
                GDot(l, pts[0].x - 4, pts[0].y - 4, ACCENT(), true);
                wchar_t mb[24];
                swprintf(mb, 24, L"%d", maxV);
                RECT mr = { pr.left + 10, pr.top + 4, pr.left + 60, pr.top + 22 };
                GText(l, g_hfSmall, TXT_DIM(), mb, mr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            } else {
                RECT er = { pr.left, pr.top, pr.right, pr.bottom };
                GText(l, g_hfSmall, TXT_DIM(), L"正在采集实时 CPS…",
                      er, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        // 统计行
        {
            wchar_t sb[64];
            long long total = g_clickCount.load(std::memory_order_relaxed);
            swprintf(sb, 64, L"累计点击  %lld", total);
            RECT r = { L.dashStats.left, L.dashStats.top, L.dashStats.right - 140, L.dashStats.bottom };
            GText(l, g_hfBody, TXT(), sb, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            ULONGLONG sec = (GetTickCount64() - s_sessionStartMs) / 1000;
            swprintf(sb, 64, L"会话  %02llu:%02llu", sec / 60, sec % 60);
            RECT r2 = { L.dashStats.right - 140, L.dashStats.top, L.dashStats.right, L.dashStats.bottom };
            GText(l, g_hfBody, TXT_DIM(), sb, r2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        // 游戏状态 (左列)
        {
            bool conn = CanAttackConnected();
            char tname[128] = {};
            GetShmTargetName(tname, sizeof(tname));
            int inGame = GetShmInGame();
            int hit = GetShmHitType();

            const wchar_t* rowNames[4] = { L"连接", L"游戏内", L"目标", L"命中" };
            std::wstring rowVal[4];
            COLORREF rowClr[4] = { TXT_DIM(), TXT_DIM(), TXT_DIM(), TXT_DIM() };
            if (conn) {
                rowVal[0] = L"已连接"; rowClr[0] = GREEN();
                rowVal[1] = inGame ? L"在游戏中" : L"主菜单";
                rowClr[1] = inGame ? GREEN() : TXT_DIM();
                if (tname[0]) {
                    wchar_t wnm[128];
                    MultiByteToWideChar(CP_UTF8, 0, tname, -1, wnm, 128);
                    rowVal[2] = wnm; rowClr[2] = TXT();
                } else {
                    rowVal[2] = L"无"; rowClr[2] = TXT_DIM();
                }
                switch (hit) {
                case 1:  rowVal[3] = L"方块"; rowClr[3] = TXT(); break;
                case 2:  rowVal[3] = L"实体"; rowClr[3] = ACCENT(); break;
                default: rowVal[3] = L"未命中"; rowClr[3] = TXT_DIM(); break;
                }
            } else {
                rowVal[0] = L"未连接";
                rowVal[1] = L"—";
                rowVal[2] = L"—";
                rowVal[3] = L"—";
            }
            for (int i = 0; i < 4; i++) {
                RECT& r = L.dashRow[i];
                RECT lr = { r.left, r.top, r.left + 76, r.bottom };
                GText(l, g_hfBody, TXT_DIM(), rowNames[i], lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                RECT vr = { r.left + 80, r.top, r.right, r.bottom };
                GText(l, g_hfBody, rowClr[i], rowVal[i].c_str(), vr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        // 分隔线
        GLFillRound(l, L.dashDiv, 0, TRACK(), (BYTE)(g_theme == Theme::Dark ? 22 : 30));
        // 方案重命名 (右列)
        {
            GText(l, g_hfSmall, TXT_DIM(), L"当前方案", L.renameLbl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& b = L.inpProfile;
            bool focus = g_inputOn && g_inputTarget == IN_PROFILE;
            GInset(l, b, 8, focus);
            if (focus) GText(l, g_hfBody, TXT(), g_inputBuf, b,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            else GText(l, g_hfBody, TXT(), g_profileNames[g_activeProfile - 1].c_str(), b,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        {
            RECT& b = L.btnRename;
            bool hover = g_hr[E_BTN_RENAME].hover;
            GButton(l, b, 10, hover, false, g_inputOn && g_inputTarget == IN_PROFILE);
            GText(l, g_hfBody, (g_inputOn && g_inputTarget == IN_PROFILE) ? RGB(255, 255, 255) : TXT(),
                  L"重命名", b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // 强调色 (右列)
        {
            GText(l, g_hfSmall, TXT_DIM(), L"强调色", L.accLbl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        for (int i = 0; i < 4; i++) {
            RECT& b = L.accDot[i];
            bool sel = (i == g_accentIdx);
            bool hover = g_hr[E_ACC0 + i].hover;
            GLFillRound(l, b, 9, ACCENT_RAW(i), 255);
            if (sel) {
                GLRing(l, { b.left - 3, b.top - 3, b.right + 3, b.bottom + 3 }, 12, 1, TXT(), 220);
                GLShadow(l, b, 9, 3, ACCENT_RAW(i), 60);
            } else if (hover) {
                GLRing(l, { b.left - 3, b.top - 3, b.right + 3, b.bottom + 3 }, 12, 1, ACCENT_RAW(i), 130);
            }
        }
        // 提示
        {
            RECT hint = { L.card[2].left + 20, L.card[2].top + S(162),
                          L.card[2].right - 20, L.card[2].top + S(184) };
            GText(l, g_hfSmall, TXT_DIM(), L"标题栏点按方案可快速切换 · 可绑定切换热键",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ================= PAGE: ADV =================
    if (g_page == PAGE_ADV) {
        // CPS 上限
        GSlider(l, SL_MAX, E_SL_MAX);
        {
            RECT& bi = L.inpMax;
            bool focus = g_inputOn && g_inputTarget == IN_CPSMAX;
            GInset(l, bi, 6, focus);
            if (focus) GText(l, g_hfBody, TXT(), g_inputBuf, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            else {
                wchar_t ibuf[8];
                swprintf(ibuf, 8, L"%d", cpsMax);
                GText(l, g_hfBody, TXT(), ibuf, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT sec = { bi.right + 6, bi.top, bi.right + 60, bi.bottom };
            GText(l, g_hfSmall, TXT_DIM(), L"次/秒", sec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // 随机 CPS
        {
            RECT& cb = L.chkRand;
            int box = cb.left;
            int by = (cb.top + cb.bottom) / 2;
            RECT sq = { box, by - 8, box + 16, by + 8 };
            bool hover = g_hr[E_CHK_RAND].hover;
            GButton(l, sq, 4, hover, false, randomCpsEnabled);
            if (randomCpsEnabled) {
                DrawCheck(dc, sq);
                GLLiftAlphaRect(l, sq);
            }
            RECT txt = { box + 22, cb.top, cb.right, cb.bottom };
            GText(l, g_hfBody, hover ? ACCENT() : TXT(), L"随机波动", txt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (randomCpsEnabled) {
                GSlider(l, SL_RAND, E_SL_RAND);
                wchar_t bufR[32];
                swprintf(bufR, 32, L"±%d CPS", randomCpsRange);
                RECT rRand = { L.track[SL_RAND].right + 8, L.card[1].top + S(56),
                               L.card[1].right - 16, L.card[1].top + S(74) };
                GText(l, g_hfBody, TXT(), bufR, rRand, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            RECT hint = { L.card[1].left + 20, L.card[1].top + S(100),
                          L.card[1].right - 20, L.card[1].top + S(122) };
            GText(l, g_hfSmall, TXT_DIM(),
                  randomCpsEnabled ? L"连点速度在 ±N CPS 内随机波动"
                                   : L"勾选后连点速度随机波动",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // 定时停止
        {
            RECT& cb = L.chkAutoStop;
            int box = cb.left;
            int by = (cb.top + cb.bottom) / 2;
            RECT sq = { box, by - 8, box + 16, by + 8 };
            bool hover = g_hr[E_CHK_AUTOSTOP].hover;
            GButton(l, sq, 4, hover, false, autoStopEnabled);
            if (autoStopEnabled) {
                DrawCheck(dc, sq);
                GLLiftAlphaRect(l, sq);
            }
            RECT txt = { box + 22, cb.top, cb.right, cb.bottom };
            GText(l, g_hfBody, hover ? ACCENT() : TXT(), L"开启", txt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT& bi = L.inpAutoStop;
            bool focus = g_inputOn && g_inputTarget == IN_AUTOSTOP;
            GInset(l, bi, 6, focus);
            if (focus) GText(l, g_hfBody, TXT(), g_inputBuf, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            else {
                wchar_t ib[16];
                swprintf(ib, 16, L"%d", autoStopSeconds);
                GText(l, g_hfBody, TXT(), ib, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT sec = { bi.right + 6, bi.top, bi.right + 44, bi.bottom };
            GText(l, g_hfSmall, TXT_DIM(), L"秒", sec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT hint = { L.card[2].left + 20, L.card[2].top + S(62),
                          L.card[2].left + 216, L.card[2].top + S(84) };
            GText(l, g_hfSmall, TXT_DIM(), L"开启后 N 秒自动停止",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // 拟人化节奏
        {
            RECT sub = { L.hm[0].left, L.card[2].top + S(12),
                         L.card[2].right - 16, L.card[2].top + S(30) };
            GText(l, g_hfSmall, TXT_DIM(), L"拟人化节奏", sub,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            static const wchar_t* hmLbl[4] = { L"均匀", L"双击", L"呼吸", L"疲劳" };
            for (int k = 0; k < 4; k++) {
                RECT& b = L.hm[k];
                bool sel = (humanizeMode == k);
                bool hover = g_hr[E_HM0 + k].hover;
                GButton(l, b, 9, hover, false, sel);
                GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT()),
                      hmLbl[k], b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        {
            GSlider(l, SL_HUM, E_SL_HUM);
            wchar_t lb[32];
            swprintf(lb, 32, L"%d", humanizeLevel);
            GText(l, g_hfSmall, TXT(), lb, L.humLevelLbl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT hint = { L.hm[0].left, L.card[2].top + S(84),
                          L.card[2].right - 16, L.card[2].top + S(104) };
            GText(l, g_hfSmall, TXT_DIM(), L"双击=短促连招 · 呼吸=CPS起伏 · 疲劳=越按越慢",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // ================= 悬停提示 (最上层) =================
    {
        // 方案芯片 (标题栏)
        for (int i = 0; i < 4; i++) {
            if (!g_hr[E_PRF0 + i].hover) continue;
            static const wchar_t* tip[] = {
                L"切换到该方案",
                L"方案保存全部设置，可在面板页重命名",
                L"可绑定切换热键（高级页）"
            };
            RenderTip(l, L.prfChip[i], tip, 3);
        }
        // 目标芯片 (状态栏)
        if (g_hr[E_CHIP_TARGET].hover) {
            char tname[128] = {};
            GetShmTargetName(tname, sizeof(tname));
            if (!CanAttackConnected()) {
                static const wchar_t* tip[] = {
                    L"未收到游戏上报",
                    L"开启攻击/放置门控后自动注入 DLL"
                };
                RenderTip(l, L.stSlot[4], tip, 2);
            } else if (tname[0]) {
                wchar_t full[140];
                MultiByteToWideChar(CP_UTF8, 0, tname, -1, full, 140);
                const wchar_t* tip[2] = { L"准星目标实体", full };
                RenderTip(l, L.stSlot[4], tip, 2);
            } else {
                static const wchar_t* tip[] = { L"准星未对准任何实体" };
                RenderTip(l, L.stSlot[4], tip, 1);
            }
        }
        if (g_page == PAGE_CLICK) {
            if (g_hr[E_BTN_CANATK].hover) {
                static const wchar_t* tip[] = {
                    L"开启后仅左键在准星对准可攻击生物时连点",
                    L"右键不受影响；未开启时左右键照常",
                    L"支持版本：1.8.8 ~ 1.21.11 几乎全版本（原版/Forge/Fabric/NeoForge）",
                    L"网易中国版（盒子）受反作弊保护，无法使用"
                };
                RenderTip(l, L.btnCanAtk, tip, 4);
            }
            if (g_hr[E_CHIP_CANATK].hover) {
                if (!CanAttackConnected()) {
                    static const wchar_t* tip[] = {
                        L"未收到游戏上报（未注入/游戏未运行）",
                        L"网易中国版受反作弊保护无法使用，此时按不可攻击处理"
                    };
                    RenderTip(l, L.canAtkChip, tip, 2);
                } else if (g_canAttack.load(std::memory_order_relaxed) == 1) {
                    static const wchar_t* tip[] = { L"准星目标可攻击，左键可连点" };
                    RenderTip(l, L.canAtkChip, tip, 1);
                } else {
                    static const wchar_t* tip[] = {
                        L"准星目标不可攻击或未对准",
                        L"左键连点已暂停"
                    };
                    RenderTip(l, L.canAtkChip, tip, 2);
                }
            }
            if (g_hr[E_BTN_CANATK_KEY].hover) {
                static const wchar_t* tip[] = {
                    L"设置开关快捷键",
                    L"按下任意键绑定 · Esc 清除"
                };
                RenderTip(l, L.btnCanAtkKey, tip, 2);
            }
            if (g_hr[E_BTN_PLACE].hover) {
                static const wchar_t* tip[] = {
                    L"开启后只有左手持有放置物（方块）时",
                    L"右键才会连点，左键照常",
                    L"支持版本：1.8.8 ~ 1.21.11 几乎全版本（原版/Forge/Fabric/NeoForge）"
                };
                RenderTip(l, L.btnPlace, tip, 3);
            }
            if (g_hr[E_CHIP_PLACE].hover) {
                if (!CanAttackConnected()) {
                    static const wchar_t* tip[] = { L"未收到游戏上报（未注入/游戏未运行）" };
                    RenderTip(l, L.placeChip, tip, 1);
                } else if (g_canPlace.load(std::memory_order_relaxed) == 1) {
                    static const wchar_t* tip[] = { L"左手所持为放置物，右键可连点" };
                    RenderTip(l, L.placeChip, tip, 1);
                } else {
                    static const wchar_t* tip[] = {
                        L"左手不是放置物或空手",
                        L"右键连点已暂停"
                    };
                    RenderTip(l, L.placeChip, tip, 2);
                }
            }
            if (g_hr[E_BTN_PLACE_KEY].hover) {
                static const wchar_t* tip[] = {
                    L"设置开关快捷键",
                    L"按下任意键绑定 · Esc 清除"
                };
                RenderTip(l, L.btnPlaceKey, tip, 2);
            }
        }
        if (g_page == PAGE_DASH) {
            static const wchar_t* accNames[4] = { L"蓝", L"紫", L"绿", L"橙" };
            for (int i = 0; i < 4; i++) {
                if (!g_hr[E_ACC0 + i].hover) continue;
                std::wstring t = std::wstring(L"强调色: ") + accNames[i];
                const wchar_t* tip[1] = { t.c_str() };
                RenderTip(l, L.accDot[i], tip, 1);
            }
            if (g_hr[E_BTN_RENAME].hover) {
                static const wchar_t* tip[] = {
                    L"重命名当前方案",
                    L"输入后回车确认 · Esc 取消"
                };
                RenderTip(l, L.btnRename, tip, 2);
            }
        }
        if (g_page == PAGE_ADV) {
            static const wchar_t* hmTip[4][2] = {
                { L"均匀: 关闭拟人化，固定 CPS", L"可与随机 CPS 叠加" },
                { L"双击连招: 短促双击 + 组间停顿", L"模拟手指点击节奏" },
                { L"呼吸波动: CPS 随时间正弦起伏", L"周期约 3.5 秒" },
                { L"疲劳递减: 按住越久越慢", L"松开后恢复原速" }
            };
            for (int k = 0; k < 4; k++) {
                if (!g_hr[E_HM0 + k].hover) continue;
                RenderTip(l, L.hm[k], hmTip[k], 2);
            }
            if (g_hr[E_SL_HUM].hover) {
                static const wchar_t* tip[] = { L"拟人化强度 (1..5)" };
                RenderTip(l, L.thumb[SL_HUM], tip, 1);
            }
        }
    }
}

// ============================================================
//  合成呈现: 清空 surface -> chrome -> chromeDyn -> content(动画) -> ULW
// ============================================================
static void Present(HWND hwnd)
{
    if (!g_layersOk) return;
    GLClear(g_surface);
    GLBlend(g_surface, g_chrome, 0, 0, 255);
    GLBlend(g_surface, g_chromeDyn, 0, 0, 255);
    int ox = AnimOffsetX();
    BYTE op = 255;
    if (g_animActive) {
        float t = g_animT;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        op = (BYTE)(150 + 105 * t);   // 滑入的同时淡入
    }
    GLBlend(g_surface, g_content, ox, 0, op);
    GLPresent(hwnd, g_surface);
}
