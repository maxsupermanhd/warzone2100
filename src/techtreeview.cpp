#include "techtreeview.h"

#include "lib/widget/button.h"

#include "hci.h"
#include "intdisplay.h"

static std::shared_ptr<W_SCREEN> techviewScreen = nullptr;
static std::shared_ptr<TechTreeThing> techviewDialog = nullptr;

std::shared_ptr<TechTreeThing> TechTreeThing::make() {
	// copypasted straight from WZScriptDebugger::make
	auto result = std::make_shared<TechTreeThing>();

	result->enableMinimizing("Script Debugger", WZCOL_FORM_TEXT);

	W_BUTINIT sButInit;
	sButInit.id = IDOBJ_CLOSE;
	sButInit.pTip = _("Close");
	sButInit.pDisplay = intDisplayImageHilight;
	sButInit.UserData = PACKDWORD_TRI(0, IMAGE_CLOSEHILIGHT, IMAGE_CLOSE);
	auto closeButton = std::make_shared<W_BUTTON>(&sButInit);
	result->attach(closeButton);
	closeButton->addOnClickHandler([](W_BUTTON& button){
		auto psParent = std::dynamic_pointer_cast<TechTreeThing>(button.parent());
		ASSERT_OR_RETURN(, psParent != nullptr, "No parent");
		widgScheduleTask([](){
			techtreeshutdown();
		});
	});
	closeButton->setCalcLayout(LAMBDA_CALCLAYOUT_SIMPLE({
		auto psParent = std::dynamic_pointer_cast<TechTreeThing>(psWidget->parent());
		ASSERT_OR_RETURN(, psParent != nullptr, "No parent");
		psWidget->setGeometry(psParent->width() - CLOSE_WIDTH, 0, CLOSE_WIDTH, CLOSE_HEIGHT);
	}));
	return result;
}

bool techtreeshutdown() {
	if (techviewScreen)
	{
		widgRemoveOverlayScreen(techviewScreen);
	}
	if (techviewDialog)
	{
		techviewDialog->deleteLater();
		techviewDialog = nullptr;
	}
	techviewScreen = nullptr;
	return true;
}

void techtreeshow() {
	techviewScreen = W_SCREEN::make();
	widgRegisterOverlayScreen(techviewScreen, std::numeric_limits<uint16_t>::max() - 3); // lol pastdue said it's alright
	techviewScreen->psForm->hide(); // from wzscriptdebug
	techviewDialog = TechTreeThing::make();
	techviewScreen->psForm->attach(techviewDialog);
}
