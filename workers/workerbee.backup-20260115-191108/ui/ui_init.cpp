#include "ui_init.h"
#include <Wire.h>
#include <FT6336U.h>
#include "../display/hosyond_lcd.h"

// Global instance
UIManager uiManager;

// Static member definitions
lv_disp_draw_buf_t UIManager::drawBuf;
lv_color_t UIManager::buf1[DISPLAY_WIDTH * 40];
lv_color_t UIManager::buf2[DISPLAY_WIDTH * 40];
lv_disp_drv_t UIManager::dispDrv;
lv_indev_drv_t UIManager::indevDrv;

// Touch controller
static FT6336U touchController(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

UIManager::UIManager() {
    tabview = nullptr;
    tabConsole = nullptr;
    tabDevices = nullptr;
    tabDashboard = nullptr;
    tabGamepad = nullptr;

    consoleList = nullptr;
    deviceGrid = nullptr;
    dashboardContainer = nullptr;
    gamepadContainer = nullptr;

    lblWifi = nullptr;
    lblHeap = nullptr;
}

void UIManager::begin() {
    Serial.println("[UI] Initializing...");

    setupDisplay();
    setupTouch();
    setupLVGL();
    createTabs();

    Serial.println("[UI] Ready");
}

void UIManager::setupDisplay() {
    Serial.println("[UI] Setting up Hosyond LCD...");

    // Initialize display using Hosyond driver
    lcd_init();

    // Clear screen to black before LVGL starts
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, 0x0000);

    lcd_set_backlight(BACKLIGHT_DEFAULT);

    Serial.printf("[UI] Display: %dx%d\n", LCD_WIDTH, LCD_HEIGHT);
}

void UIManager::setupTouch() {
    Serial.println("[UI] Setting up touch controller...");

    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    touchController.begin();

    Serial.println("[UI] Touch ready (FT6336U)");
}

void UIManager::setupLVGL() {
    Serial.println("[UI] Initializing LVGL...");

    lv_init();

    // Initialize draw buffer (double buffered)
    lv_disp_draw_buf_init(&drawBuf, buf1, buf2, DISPLAY_WIDTH * 40);

    // Initialize display driver
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = DISPLAY_WIDTH;
    dispDrv.ver_res = DISPLAY_HEIGHT;
    dispDrv.flush_cb = displayFlush;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    // Initialize input device (touch)
    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchpadRead;
    lv_indev_drv_register(&indevDrv);

    // Set dark theme
    lv_theme_t* theme = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_GREY),
        true,  // Dark mode
        LV_FONT_DEFAULT
    );
    lv_disp_set_theme(lv_disp_get_default(), theme);

    Serial.println("[UI] LVGL initialized");
}

void UIManager::displayFlush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    lcd_set_window(area->x1, area->y1, area->x2, area->y2);
    lcd_push_colors((uint16_t*)&color_p->full, w * h);

    lv_disp_flush_ready(drv);
}

void UIManager::touchpadRead(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    FT6336U_TouchPointType tp = touchController.scan();

    if (tp.touch_count > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        // Remap touch for landscape mode
        // Touch panel is 240x320, display is 320x240
        data->point.x = tp.tp[0].y;
        data->point.y = 239 - tp.tp[0].x;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void UIManager::update() {
    lv_timer_handler();
}

void UIManager::setBrightness(uint8_t level) {
    lcd_set_backlight(level);
}

void UIManager::createTabs() {
    Serial.println("[UI] Creating tabview...");

    // Create main tabview
    tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 35);

    // Set tab bar style
    lv_obj_set_style_bg_color(lv_tabview_get_tab_btns(tabview),
                              lv_color_hex(0x1a1a1a), 0);

    // Add tabs (2-tab layout: System + Input)
    tabDashboard = lv_tabview_add_tab(tabview, "System");
    tabGamepad = lv_tabview_add_tab(tabview, "Input");

    // Create content
    createDashboardScreen();
    createGamepadScreen();

    Serial.println("[UI] Tabs created");
}

void UIManager::createConsoleScreen() {
    consoleList = lv_list_create(tabConsole);
    lv_obj_set_size(consoleList, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(consoleList, lv_color_hex(0x0d0d0d), 0);
    lv_obj_set_style_pad_all(consoleList, 4, 0);

    lv_obj_t* btn = lv_list_add_text(consoleList, "Waiting for MQTT...");
    lv_obj_set_style_text_color(btn, lv_color_hex(0x666666), 0);
}

void UIManager::createDevicesScreen() {
    deviceGrid = lv_obj_create(tabDevices);
    lv_obj_set_size(deviceGrid, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(deviceGrid, lv_color_hex(0x0d0d0d), 0);
    lv_obj_set_style_pad_all(deviceGrid, 4, 0);

    lv_obj_set_layout(deviceGrid, LV_LAYOUT_GRID);

    static lv_coord_t colDefs[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rowDefs[] = {80, 80, 80, 80, 80, LV_GRID_TEMPLATE_LAST};

    lv_obj_set_grid_dsc_array(deviceGrid, colDefs, rowDefs);

    lv_obj_t* label = lv_label_create(deviceGrid);
    lv_label_set_text(label, "Discovering devices...");
    lv_obj_set_style_text_color(label, lv_color_hex(0x666666), 0);
    lv_obj_set_grid_cell(label, LV_GRID_ALIGN_CENTER, 0, 2, LV_GRID_ALIGN_CENTER, 0, 1);
}

void UIManager::createDashboardScreen() {
    dashboardContainer = lv_obj_create(tabDashboard);
    lv_obj_set_size(dashboardContainer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dashboardContainer, lv_color_hex(0x0d0d0d), 0);
    lv_obj_set_flex_flow(dashboardContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(dashboardContainer, 8, 0);
    lv_obj_set_style_pad_gap(dashboardContainer, 8, 0);

    lv_obj_t* title = lv_label_create(dashboardContainer);
    lv_label_set_text(title, "Worker Bee Stats");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lblWifi = lv_label_create(dashboardContainer);
    lv_label_set_text(lblWifi, "WiFi: --");
    lv_obj_set_style_text_color(lblWifi, lv_palette_main(LV_PALETTE_CYAN), 0);

    lblHeap = lv_label_create(dashboardContainer);
    lv_label_set_text(lblHeap, "Heap: --KB");
    lv_obj_set_style_text_color(lblHeap, lv_palette_main(LV_PALETTE_GREEN), 0);
}

void UIManager::updateSystemStats(bool wifiConnected, int rssi, const char* ip, uint32_t heap) {
    if (lblWifi == nullptr) return;

    char buf[64];
    if (wifiConnected) {
        snprintf(buf, sizeof(buf), "WiFi: %s (%d dBm)", ip, rssi);
        lv_obj_set_style_text_color(lblWifi, lv_palette_main(LV_PALETTE_CYAN), 0);
    } else {
        snprintf(buf, sizeof(buf), "WiFi: Disconnected");
        lv_obj_set_style_text_color(lblWifi, lv_palette_main(LV_PALETTE_RED), 0);
    }
    lv_label_set_text(lblWifi, buf);

    snprintf(buf, sizeof(buf), "Heap: %luKB", heap / 1024);
    lv_label_set_text(lblHeap, buf);
}

void UIManager::createGamepadScreen() {
    gamepadContainer = lv_obj_create(tabGamepad);
    lv_obj_set_size(gamepadContainer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(gamepadContainer, lv_color_hex(0x0d0d0d), 0);
    lv_obj_set_flex_flow(gamepadContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(gamepadContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(gamepadContainer, 16, 0);

    lv_obj_t* title = lv_label_create(gamepadContainer);
    lv_label_set_text(title, "BLE Gamepad");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t* statusLed = lv_led_create(gamepadContainer);
    lv_obj_set_size(statusLed, 30, 30);
    lv_led_set_color(statusLed, lv_palette_main(LV_PALETTE_RED));
    lv_led_off(statusLed);

    lv_obj_t* statusText = lv_label_create(gamepadContainer);
    lv_label_set_text(statusText, "Not Connected");
    lv_obj_set_style_text_color(statusText, lv_color_hex(0x888888), 0);

    lv_obj_t* instr = lv_label_create(gamepadContainer);
    lv_label_set_text(instr, "Pair via Bluetooth:\n\"WorkerBee Gamepad\"");
    lv_obj_set_style_text_align(instr, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(instr, lv_color_hex(0x666666), 0);
}

// Tab navigation
void UIManager::showConsoleTab() { lv_tabview_set_act(tabview, 0, LV_ANIM_ON); }
void UIManager::showDevicesTab() { lv_tabview_set_act(tabview, 1, LV_ANIM_ON); }
void UIManager::showDashboardTab() { lv_tabview_set_act(tabview, 2, LV_ANIM_ON); }
void UIManager::showGamepadTab() { lv_tabview_set_act(tabview, 3, LV_ANIM_ON); }

// Getters
lv_obj_t* UIManager::getConsoleList() { return consoleList; }
lv_obj_t* UIManager::getDeviceGrid() { return deviceGrid; }
lv_obj_t* UIManager::getDashboardContainer() { return dashboardContainer; }
lv_obj_t* UIManager::getGamepadContainer() { return gamepadContainer; }
