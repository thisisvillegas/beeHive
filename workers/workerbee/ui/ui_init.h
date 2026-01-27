#ifndef UI_INIT_H
#define UI_INIT_H

#include <Arduino.h>
#include <lvgl.h>
#include "../config.h"

/**
 * UI Manager - LVGL + Hosyond LCD Integration
 */

class UIManager {
public:
    UIManager();

    void begin();
    void update();
    void setBrightness(uint8_t level);

    // Button callback
    typedef void (*ButtonCallback)(int buttonNum);
    void setButtonCallback(ButtonCallback cb);

    // Update system stats on display
    void updateSystemStats(bool wifiConnected, int rssi, const char* ip, uint32_t heap,
                           bool mqttConnected, const char* mqttBroker, uint16_t mqttPort,
                           const char* deviceId, uint32_t uptime, const char* firmware,
                           bool companionOnline, const char* companionId);

    // Add log message to logs tab
    void addLog(const char* msg);

    // Tab navigation
    void showConsoleTab();
    void showDevicesTab();
    void showDashboardTab();
    void showGamepadTab();

    // Get LVGL objects for updating
    lv_obj_t* getConsoleList();
    lv_obj_t* getDeviceGrid();
    lv_obj_t* getDashboardContainer();
    lv_obj_t* getGamepadContainer();

private:
    // LVGL display buffer
    static lv_disp_draw_buf_t drawBuf;
    static lv_color_t buf1[DISPLAY_WIDTH * 40];
    static lv_color_t buf2[DISPLAY_WIDTH * 40];

    // LVGL drivers
    static lv_disp_drv_t dispDrv;
    static lv_indev_drv_t indevDrv;

    // UI elements
    lv_obj_t* tabview;
    lv_obj_t* tabConsole;
    lv_obj_t* tabDevices;
    lv_obj_t* tabDashboard;
    lv_obj_t* tabGamepad;
    lv_obj_t* tabButtons;

    lv_obj_t* consoleList;
    lv_obj_t* deviceGrid;
    lv_obj_t* dashboardContainer;
    lv_obj_t* gamepadContainer;
    lv_obj_t* buttonsContainer;

    // System stats labels
    lv_obj_t* lblWifi;
    lv_obj_t* lblMqtt;
    lv_obj_t* lblHeap;
    lv_obj_t* lblDeviceId;
    lv_obj_t* lblUptime;
    lv_obj_t* lblFirmware;
    lv_obj_t* lblCompanion;
    lv_obj_t* lblFps;

    // FPS tracking
    uint32_t frameCount;
    uint32_t lastFpsUpdate;
    uint16_t currentFps;

    // Button callback
    static ButtonCallback buttonCallback;
    static void buttonEventHandler(lv_event_t* e);

    // Setup functions
    void setupDisplay();
    void setupTouch();
    void setupLVGL();
    void createTabs();
    void createConsoleScreen();
    void createDevicesScreen();
    void createDashboardScreen();
    void createGamepadScreen();
    void createButtonsScreen();

    // LVGL callbacks
    static void displayFlush(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);
    static void touchpadRead(lv_indev_drv_t* drv, lv_indev_data_t* data);
};

extern UIManager uiManager;

#endif // UI_INIT_H
