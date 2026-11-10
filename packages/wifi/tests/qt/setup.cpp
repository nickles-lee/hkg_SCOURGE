/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors */
/* SPDX-License-Identifier: AGPL-3.0-only */
#include <QtQuickTest/quicktest.h>
#include <QQmlEngine>
#include <QQmlContext>

class Model : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isWideScr MEMBER wide NOTIFY isWideScrChanged)
    Q_PROPERTY(bool isAtOtherApp MEMBER other NOTIFY isAtOtherAppChanged)
    Q_PROPERTY(bool sysPopupShowing MEMBER popup NOTIFY sysPopupShowingChanged)
    Q_PROPERTY(bool requestHomeByPP MEMBER home NOTIFY requestHomeByPPChanged)
    Q_PROPERTY(int currentScreen MEMBER screen NOTIFY currentScreenChanged)
    Q_PROPERTY(int currentHomeAdvancedPageIdx MEMBER page NOTIFY currentHomeAdvancedPageIdxChanged)
public:
    bool wide = true, other = false, popup = false, home = false;
    int screen = 0, page = 0;
signals:
    void isWideScrChanged(); void isAtOtherAppChanged(); void sysPopupShowingChanged();
    void requestHomeByPPChanged(); void currentScreenChanged(); void currentHomeAdvancedPageIdxChanged();
    void buttonBackClicked(); void switchToOtherApp();
};

class Constants : public QObject {
    Q_OBJECT
    Q_PROPERTY(int SCREEN_WIDTH MEMBER width CONSTANT)
    Q_PROPERTY(int MENU_BAR_Y MEMBER top CONSTANT)
public:
    int width = 1280, top = 90;
};

class Setup : public QObject {
    Q_OBJECT
    Model model;
    Constants constants;
public slots:
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->rootContext()->setContextProperty("AppHomeModel", &model);
        engine->rootContext()->setContextProperty("ConstantModel", &constants);
        engine->rootContext()->setContextProperty("DEVICE_WIDTH", 1920);
        engine->rootContext()->setContextProperty("DEVICE_HEIGHT", 720);
    }
};
QUICK_TEST_MAIN_WITH_SETUP(wifi_overlay, Setup)
#include "setup.moc"
