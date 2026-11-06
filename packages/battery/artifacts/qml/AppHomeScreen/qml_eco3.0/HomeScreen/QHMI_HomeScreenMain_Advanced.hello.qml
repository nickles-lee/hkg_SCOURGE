// HELLO_PAYLOAD_260128
// BATTERY_PAYLOAD_260128
import QtQuick 2.3
import "../component"
import "../component/common"
import HMIEVENTS 1.0
import AppHome_Enum 1.0

QRootScreen {
    property string _prefixImg: defineModel.prefixImg99
    property string _prefixOtherImg: defineModel.prefixImgHome
    property bool isWideScreen: /*AppHomeModel.isFullMode &&*/ AppHomeModel.isWideScr
    property bool customizeModeHK_PAGE0: false
    property bool customizeModeHK_PAGE1: false
    property bool customizeModeHK_PAGE2: false
    property bool customizeModeHK_PAGE3: false
    property bool customizeModeHK_PAGE4: false
    property bool mouseAreaPressed:false
    property bool hkPressd: false
    property var homePageItem: getModelList()
    property var optMenuModelData:[
        {indexOptItem: AppHome_Enum.E_OPT_DISPLAY_OFF,  label: TranslationsModel.STR_HOME_TITLE_DIS_OFF, isItemSelect: false, isEnabled: true},
        {indexOptItem: AppHome_Enum.E_OPT_EDIT_HOME_MENU,   label: TranslationsModel.STR_HOME_EDIT_MENUS, isItemSelect: false, isEnabled: true},
        {indexOptItem: AppHome_Enum.E_OPT_MANUAL,  label: TranslationsModel.STR_HOME_MANUAL + " > " +TranslationsModel.STR_HOME_SCREEN, isItemSelect: false, isEnabled: true}
    ]

    // For Menu items
    readonly property int k_nbrOfRows: 2
    property string prefixImg: defineModel.prefixImg99
    property int k_itemWidth:  306
    property int k_itemHeight: 249
    property int k_itemWidth_custo: 272
    property int k_itemWidth_custo_s: 248
    property int k_itemHeight_custo: 207
    property int k_menuPageWidth : isWideScreen? k_itemWidth * 6: k_itemWidth * 4
    property int k_menuPageHeight : k_itemHeight*2
    property int k_menuPageWidthCustomize : isWideScreen ? 1636 : 999
    property int k_menuPageHeightCustomize : 417
    property int k_nbrOfColumns: isWideScreen ? 6 : 4
    property int nbrItemsPerPage: k_nbrOfRows*k_nbrOfColumns
    property alias listPosY: homepage.y           // position Y of the list
    property int totalPages: 0    // total menu pages
    property int totalPagesNormalMode: 0
    property int listType : AppHome_Enum.E_LIST_ALL_MENUS
    property bool custoBGVisible: true
    property int currentPageIndex: 0
    property int menuDummyItemIdx: -1
    property int srcPageIndex: 0
    property int dstPageIndex: 0
    property bool isImagePSingleImg: true
    property int maxNumPages: -1
    property bool listChange: false
    property bool isAllMenuScreen: true // Is used for not conflict the logic of constructMenuModel()

    property int lastDropIndex: -1
    property int lastDropPage: -1
    property int draggingIndex
    property int draggingPage
    property var _listModel: AppHomeModel.listHomePage
    signal pageIndexChanged(int index)
    signal swapMenuItem(int dragItemIdx, int dropItemIdx)
    signal updateMenuItem(int dragItemPage, int dragItemIndex, int dropItemPage,int dropItemIndex)
    signal clickOutSide();
    signal indexChange();
    property bool isButtonWidgetActive: false
    property bool isCurrentIndexActive: false
    property bool isListFlicking: false

    ListModel{
        id: listModelDisplay
        Component.onCompleted: {
            constructMenuModel()
            homePageItem = getModelList()
        }
    }

    function getModelList(){
        if(totalPagesNormalMode === 3){
            return [pluginPage, listPage1, listPage2, listPage3]
        }else if(totalPagesNormalMode === 2){
            return [pluginPage, listPage1, listPage2]
        }else if(totalPagesNormalMode === 1){
            return [pluginPage, listPage1]
        }else{
            return []
        }
    }

    function constructMenuModel(){
        console.log("[3] call constructMenuModel")
        AppHomeModel.prepareAdvancedDisplayList(nbrItemsPerPage);

        updateDisplayList()
        indexChange()
    }

    function updateDisplayList()
    {
        var index = AppHomeModel.checkTotalPageFromMD()

        // Checking condition to prevent reload list.
        if (totalPagesNormalMode !== index)
        {
            totalPagesNormalMode = index
            AppHomeModel.numberPage = totalPagesNormalMode
        }
    }

    function emptySlots(pageModel) {
        var slots = []
        if (!pageModel) return slots
        var n = pageModel.getCount ? pageModel.getCount() : pageModel.count
        for (var i = 0; i < n; i++) {
            var item = pageModel.getItem ? pageModel.getItem(i) : null
            var isEmpty = true
            if (item) {
                if (typeof item.isEditingVisible === "function") {
                    isEmpty = !item.isEditingVisible()
                } else if (typeof item.visible !== "undefined") {
                    isEmpty = item.visible !== true
                } else {
                    isEmpty = false
                }
            }
            if (isEmpty) slots.push(i)
        }
        for (var j = n; j < nbrItemsPerPage; j++) slots.push(j)
        return slots
    }

    // menuPageN already sits below the top menu bar; callers add row offsets.
    function firstEmptySlot(customIndex) {
        var remaining = customIndex
        for (var p = 0; p < totalPagesNormalMode; p++) {
            var slots = emptySlots(getPageDisplayListModel(p))
            if (remaining < slots.length) {
                return { page: p, index: slots[remaining] }
            }
            remaining -= slots.length
        }
        return null
    }

    function firstEmptyIndex(pageModel) {
        var slots = emptySlots(pageModel)
        return slots.length ? slots[0] : -1
    }


    function getPageDisplayListModel(index)
    {

        if (index === 0)
        {
            return AllMenuPage1Display;
        }
        else if (index === 1)
        {
            return AllMenuPage2Display;
        }
        else if (index === 2)
        {
            return AllMenuPage3Display;
        }
        return NULL;

    }

    function resetAllPages()
    {
        AllMenuPage1Display.resetData();
        AllMenuPage2Display.resetData();
        AllMenuPage3Display.resetData();
    }

    function updateItemTonedown(itemIds, itemCount, lstTarget, itemTonedown){
        if( lstTarget !== listType ){
            return;
        }
        var listItem = []
        for (var i = 0; i < itemCount; i++)
        {
            listItem.push(itemIds[i])
        }
        AppHomeModel.updateItemTonedown(listItem, itemCount, itemTonedown)
    }
    function updateItemVisible(itemId, visible)
    {
        console.log("itemId: ", itemId, " visible: ",visible);
        AppHomeModel.updateItemVisible(itemId, visible)
    }

    id: root
    isEnable: !AppHomeModel.sysPopupShowing
    width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
    height: DEVICE_HEIGHT
    focus: true

    Q_ThemeImage {
        id: statusBarBG
        width: parent.width
        height: sourceSize.height
        themeSource: defineModel.co_bg_statusbar
        fillMode: Image.TileHorizontally
    }

    Q_ThemeImage {
        anchors.bottom: parent.bottom
        id: home_bg_image
        themeSource: AppHomeModel.isWideScr? defineModel.home_main_bg_01 : defineModel.home_main_bg_02
        anchors.horizontalCenter: parent.horizontalCenter
    }

    Component {
        id: pluginPage

        Item {
            width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
            height: DEVICE_HEIGHT
            objectName: "itemWidgetPlaceHolder"     // Placeholder for C++ to later widget object injection

            // Connect to qml object signal.
            Connections {
                target: AdvancedWidgetQMLInstance
                ignoreUnknownSignals: true
                onSetButtonWidgetActive: {
                    console.log("[3] isButtonWidgetActive = ", value)
                    isButtonWidgetActive = value
                }
            }

            Component.onCompleted: {
                console.log("[3] pluginPage loaded.")
                AppHomeEngine.slotReqInjectAdvancedWidget();
            }
        }
    }

    Component {
        id: listPage1

        Item {
            id: itemPage1
            y: (ConstantModel.MENU_BAR_Y) + (ConstantModel.MENU_BAR_Y) / 2
            property int allMenuIndex: 0

            width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
            height: DEVICE_HEIGHT
            Item {
                id: gridDelegate
                width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
                height: k_menuPageHeight

                QMenuPageNoDrag{
                    id: menuPage
                    imgPath: prefixImg
                    pageIndex: allMenuIndex
                    pageModel: AllMenuPage1.allMenuScreenOwner == isAllMenuScreen ? getPageDisplayListModel(allMenuIndex) : undefined
                    width: k_menuPageWidth
                    height: k_menuPageHeight
                    parentView: homepage
                    rootView: menuPage
                    cellWidth: k_itemWidth
                    cellHeight: k_itemHeight
                    anchors.horizontalCenter: parent.horizontalCenter
                    interactive: false
                    onItemClicked: {
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)
                    }
                    onItemPressAndHold: {
                        AppHomeModel.fromPage0 = false
                        AppHomeModel.editHomeWidgetPos = 0
                        QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED)
                    }
                }

                HelloTile {
                    id: helloTile1
                    property var slot: firstEmptySlot(0)
                    visible: slot !== null && slot.page === 0
                    x: menuPage.x + (slot !== null && slot.page === 0 ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: menuPage.y + (slot !== null && slot.page === 0 ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    onClicked: helloPopup.visible = true
                }
                BatteryTile {
                    id: batteryTile1
                    property var slot: firstEmptySlot(1)
                    visible: slot !== null && slot.page === 0
                    x: menuPage.x + (slot !== null && slot.page === 0 ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: menuPage.y + (slot !== null && slot.page === 0 ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    onClicked: batteryOverlay.open()
                }




            }
        }
    }

    Component {
        id: listPage2
        Item {
            id: itemPage2
            y: (ConstantModel.MENU_BAR_Y) + (ConstantModel.MENU_BAR_Y) / 2
            property int allMenuIndex: 1

            width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
            height: DEVICE_HEIGHT
            Item {
                id: gridDelegate2
                width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
                height: k_menuPageHeight

                QMenuPageNoDrag{
                    id: menuPage2
                    imgPath: prefixImg
                    pageIndex: allMenuIndex
                    pageModel: AllMenuPage1.allMenuScreenOwner == isAllMenuScreen ? getPageDisplayListModel(allMenuIndex) : undefined
                    width: k_menuPageWidth
                    height: k_menuPageHeight
                    parentView: homepage
                    rootView: menuPage2
                    cellWidth: k_itemWidth
                    cellHeight: k_itemHeight
                    anchors.horizontalCenter: parent.horizontalCenter
                    interactive: false
                    onItemClicked: {
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)
                    }
                    onItemPressAndHold: {
                        AppHomeModel.fromPage0 = false
                        AppHomeModel.editHomeWidgetPos = 0
                        QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED)
                    }
                }

                HelloTile {
                    id: helloTile2
                    property var slot: firstEmptySlot(0)
                    visible: slot !== null && slot.page === 1
                    x: menuPage2.x + (slot !== null && slot.page === 1 ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: menuPage2.y + (slot !== null && slot.page === 1 ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    onClicked: helloPopup.visible = true
                }
                BatteryTile {
                    id: batteryTile2
                    property var slot: firstEmptySlot(1)
                    visible: slot !== null && slot.page === 1
                    x: menuPage2.x + (slot !== null && slot.page === 1 ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: menuPage2.y + (slot !== null && slot.page === 1 ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    onClicked: batteryOverlay.open()
                }



            }
        }
    }

    Component {
        id: listPage3
        Item {
            id: itemPage3
            y: (ConstantModel.MENU_BAR_Y) + (ConstantModel.MENU_BAR_Y) / 2
            property int allMenuIndex: 2

            width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
            height: DEVICE_HEIGHT
            Item {
                id: gridDelegate3
                width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
                height: k_menuPageHeight

                QMenuPageNoDrag{
                    id: menuPage3
                    isFullMode: true
                    imgPath: prefixImg
                    pageIndex: allMenuIndex
                    pageModel: AllMenuPage1.allMenuScreenOwner == isAllMenuScreen ? getPageDisplayListModel(allMenuIndex) : undefined
                    width: k_menuPageWidth
                    height: k_menuPageHeight
                    parentView: homepage
                    rootView: menuPage3
                    cellWidth: k_itemWidth
                    cellHeight: k_itemHeight
                    anchors.horizontalCenter: parent.horizontalCenter
                    interactive: false
                    onItemClicked: {
                        QML_Handler.qmlSendEvtKey(HMIEVENTS.EVT_MAIN_LIST_ITEMCLICKED, _itemID)
                    }
                    onItemPressAndHold: {
                        AppHomeModel.fromPage0 = false
                        AppHomeModel.editHomeWidgetPos = 0
                        QML_Handler.qmlChangeTopMsg(HMIEVENTS.EVT_GOTO_SCREEN_ALL_MENUS_REARRANGE_ADVANCED)
                    }
                }

                HelloTile {
                    id: helloTile3
                    property var slot: firstEmptySlot(0)
                    visible: slot !== null && slot.page === 2
                    x: menuPage3.x + (slot !== null && slot.page === 2 ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: menuPage3.y + (slot !== null && slot.page === 2 ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    onClicked: helloPopup.visible = true
                }
                BatteryTile {
                    id: batteryTile3
                    property var slot: firstEmptySlot(1)
                    visible: slot !== null && slot.page === 2
                    x: menuPage3.x + (slot !== null && slot.page === 2 ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: menuPage3.y + (slot !== null && slot.page === 2 ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    onClicked: batteryOverlay.open()
                }




            }
        }
    }

    QListViewHomePage {
        id: homepage
        objectName: "lsvHomePage"
        property bool isAnimating: false
        x: 0
        width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT
        anchors.verticalCenter: parent.verticalCenter
        orientation: ListView.Horizontal
        model: totalPagesNormalMode == 0 ? 0: totalPagesNormalMode + 1
        snapMode: ListView.SnapOneItem
        highlightRangeMode: ListView.StrictlyEnforceRange
        preferredHighlightBegin: 0
        preferredHighlightEnd: 0
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 35000
        //        cacheBuffer : homepage.width*(totalPagesNormalMode + 1) + 1
        displayMarginBeginning: homepage.width*(totalPagesNormalMode + 1)
        clip: false
        spacing: 0
        interactive: !isButtonWidgetActive
        pressDelay: currentIndex == 0 ? 0 : 100
        delegate: Item {
            id: pageId
            width: AppHomeModel.isWideScr? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
            height: DEVICE_HEIGHT
            Loader {
                id: itemLoader
                enabled: !homepage.flicking
                anchors.centerIn: parent
                sourceComponent: homePageItem[index]
//                asynchronous: AppHomeModel.currentHomeAdvancedPageIdx == index || index == 0 ? false : true;
            }

            visible: (!homepage.isAnimating || index == AppHomeModel.currentHomeAdvancedPageIdx) && itemLoader.status == Loader.Ready
        }
        property bool isAready : false

        onCurrentIndexChanged: {
            if(root.isCurrentIndexActive){
                AppHomeModel.currentHomeAdvancedPageIdx = homepage.currentIndex
            }
            AppHomeEngine.advancedModePageChange(homepage.currentIndex)
        }
        onOpacityChanged: {
            if (opacity < 1 && homepage.isAnimating == false)
            {
                if (homepage.isAnimating == false)
                {
                    console.log("[NTH] isAnimating true")
                    homepage.isAnimating = true
                }
            }
            else
            {
                if (homepage.isAnimating)
                {
                    console.log("[NTH] isAnimating false")
                    homepage.isAnimating = false
                }
            }
        }
        onContentXChanged: {
            AppHomeModel.homeMainContentX = contentX
        }

        onMovementStarted: {
            reqShowIndicator()
            isListFlicking = true
        }

        onMovementEnded: {
            startHideIndicator()
            isListFlicking = false
        }
    }

    QMainScreenAnimation {
        id: right_left_animation
        positionTarget: homepage
        opacityTarget: homepage
    }

    Row{
        id: indicatorGroup
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
//        anchors.topMargin: -20
        width: ((totalPagesNormalMode + 1) * 39) + (totalPagesNormalMode * 9)//totalPagesNormalMode * 85 + 12
        height: 26
        spacing: 9
        visible: true
        Repeater {
            model: (totalPagesNormalMode + 1)
            Q_ThemeImage {
//                anchors.verticalCenter: parent.verticalCenter
                themeSource: (index === (AppHomeModel.currentHomeAdvancedPageIdx) ? defineModel.home_ic_indicator_w_s : defineModel.home_ic_indicator_w_n)
            }
        }
    }

    Timer {
        id: timerHideIndicator
        interval: 5000
        running: false
        onTriggered: {
            indicatorGroup.visible = false
        }
    }
    OpacityAnimator{
        id: animationHideIndicator
        target: indicatorGroup;
        from: 1
        to:0
        duration: 5000
        running: false
    }
    function startHideIndicator() {
        if(animationHideIndicator.running) {
            animationHideIndicator.restart()
        } else {
            animationHideIndicator.start()
        }
    }

    function reqShowIndicator() {
        if(animationHideIndicator.running)
        {
            animationHideIndicator.stop()
        }
        if((totalPagesNormalMode + 1) > 1) {
            indicatorGroup.opacity = 1
        } else  {
            indicatorGroup.opacity = 0
        }
    }

    Connections {
        target: AppHomeModel
        onRequestUpdateAllMenu: {
            console.log("[3] Advanced Home: onRequestUpdateAllMenu")
            var indexCurrent = AppHomeModel.currentHomeAdvancedPageIdx

            if (AllMenuPage1.allMenuScreenOwner && !AppHomeEngine.getIsDrawing())
            {
                constructMenuModel()
                homePageItem = getModelList()
            }
            else
            {
                console.log("[3] Not allmenu owner -> ignore.")
            }
            homepage.currentIndex = AppHomeModel.currentHomeAdvancedPageIdx
            homepage.positionViewAtIndex(indexCurrent,ListView.Beginning)
        }
        onMenuListUpdated: {
            if( lstTarget !== listType ){
                return;
            }
            console.log("[3] Advanced Home: onMenuListUpdated")
            goHistorypage()
            updateItemTonedown(itemIds, itemCount, lstTarget, itemToneDown)
        }
        onAllMenuEnableUpdated: {
            console.log("[3] Advanced Home: onAllMenuEnableUpdated")
            updateItemTonedown(itemIds, itemCount, lstTarget, itemToneDown)
        }

        onRequestUpdatePageIndexChanged: {
            console.log("[3] Advanced Home: onRequestUpdatePageIndexChanged")
            homepage.currentIndex = AppHomeModel.currentHomeAdvancedPageIdx
            homepage.positionViewAtIndex(homepage.currentIndex, ListView.Contain)
            AppHomeEngine.requestHideGlobalPopup()
        }

        onKeepCurrentScreen: {
            console.log("[3] isListFlicking: " + isListFlicking)
            if(isListFlicking)
            {
                console.log("[3] onKeepCurrentScreen: " + homepage.currentIndex)
                isListFlicking = false
                homepage.positionViewAtIndex(homepage.currentIndex, ListView.Beginning)
            }
        }

        onSysPopupShowingChanged:
        {
            console.log("[3] onSysPopupShowingChanged: " + isListFlicking)
            if(AppHomeModel.sysPopupShowing && isListFlicking)
            {
                homepage.returnToBounds()
                isListFlicking = false
            }
        }

        onReqShowIndicator:
        {
            console.log("[3] onReqShowIndicator")
            reqShowIndicator()
            startHideIndicator()
        }

//        onIsAtOtherAppChanged: {
//            console.log("[3] Main Home Advance: " + homepage.currentIndex)
//            homepage.positionViewAtIndex(homepage.currentIndex, ListView.Beginning)
//            if(isListFlicking)
//            {
//                isListFlicking = false
//            }
//        }
    }

    HelloPopup {
        id: helloPopup
        visible: false
        onClosed: visible = false
    }

    Connections {
        target: AppHomeModel
        onButtonBackClicked: {
            if (helloPopup.visible) {
                helloPopup.visible = false
            }
        }
    }

    BatteryPage {
        id: batteryOverlay
        x: 0
        y: ConstantModel.MENU_BAR_Y
        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
        z: 2000
        onGoHome: close()
    }

    Component.onCompleted: {
        AppHomeModel.screenNotSupportSS = true
        AppHomeModel.reqLoadWidget()
        AppHomeModel.currentSetupPageIdx = 0
        AppHomeModel.currentMenuPageIdx = 0
        AppHomeModel.currentMenuMediaGatePageIdx = 0
        AppHomeModel.listOptMenu = Qt.binding(function(){return optMenuModelData})
        if(!AppHomeModel.preventScreenAnimation)
        {
            console.log("[3] Start timer to hide the Indicator")
            reqShowIndicator()
            startHideIndicator()
        }
        goHistorypage()
        if(AppHomeModel.currentHomeAdvancedPageIdx !==0 && !AppHomeModel.requestHomeByPP){
            right_left_animation.startAnimation()
        }
        else if (AppHomeModel.requestHomeByPP)
        {
            AppHomeModel.requestHomeByPP = false
        }
        AppHomeModel.homeMainContentX = homepage.contentX;
    }

    function goHistorypage(){
        root.isCurrentIndexActive = false
        homepage.positionViewAtIndex(2,ListView.Beginning)
        if(AppHomeModel.currentHomeAdvancedPageIdx <= totalPagesNormalMode)
        {
            homepage.currentIndex = AppHomeModel.currentHomeAdvancedPageIdx
            homepage.positionViewAtIndex(AppHomeModel.currentHomeAdvancedPageIdx,ListView.Beginning)
        }
        else
        {
            AppHomeModel.currentHomeAdvancedPageIdx = totalPagesNormalMode
            homepage.currentIndex = AppHomeModel.currentHomeAdvancedPageIdx
            homepage.positionViewAtIndex(AppHomeModel.currentHomeAdvancedPageIdx,ListView.Beginning)
        }
        root.isCurrentIndexActive = true
    }
}
