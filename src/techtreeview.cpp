#include "techtreeview.h"

#include "lib/widget/button.h"

#include "hci.h"
#include "intdisplay.h"
#include "frontend.h"
#include "multiint.h"
#include "intdisplay.h"
#include "research.h"

#define TECHTREEVIEW_WINDOW_HEIGHT_MAX 440
// #define SCRIPTDEBUG_BUTTON_HEIGHT 24
// #define SCRIPTDEBUG_ROW_HEIGHT 24
#define ACTION_BUTTON_ROW_SPACING 10
// #define ACTION_BUTTON_SPACING 10

#define TITLE_TOP_PADDING 5
#define TITLE_HEIGHT 24
#define TITLE_BOTTOM (TITLE_TOP_PADDING + TITLE_HEIGHT)
#define TAB_BUTTONS_HEIGHT 24
#define TAB_BUTTONS_PADDING 10
#define STAT_GAP			2
#define STAT_BUTWIDTH		60
#define STAT_BUTHEIGHT		46

const PIELIGHT WZCOL_DEBUG_FILL_COLOR = pal_RGBA(25, 0, 110, 220);
const PIELIGHT WZCOL_DEBUG_FILL_COLOR_DARK = pal_RGBA(10, 0, 70, 250);
const PIELIGHT WZCOL_DEBUG_BORDER_LIGHT = pal_RGBA(255, 255, 255, 80);
const PIELIGHT WZCOL_DEBUG_INDETERMINATE_PROGRESS_COLOR = pal_RGBA(240, 240, 255, 180);

static std::shared_ptr<W_SCREEN> techviewScreen = nullptr;
static std::shared_ptr<TechTreeThing> techviewDialog = nullptr;

BASE_OBJECT *lastSelectedLab = nullptr;

static std::shared_ptr<W_BUTTON> makeCornerButton(const char* text);
static std::shared_ptr<W_BUTTON> makeTechButton();
static RESEARCH* ExtractResearchFromLab(BASE_OBJECT *psObj);

struct TechButtonData {
	std::shared_ptr<IntStatsButton> tech;
};

TechTreeThing::TechTreeThing() {

}

TechTreeThing::~TechTreeThing() {

}

void TechTreeThing::display(int xOffset, int yOffset)
{
	int x0 = x() + xOffset;
	int y0 = y() + yOffset;
	int x1 = x0 + width();
	int y1 = y0 + height();

	// draw "drop-shadow"
	int dropShadowX0 = std::max<int>(x0 - 6, 0);
	int dropShadowY0 = std::max<int>(y0 - 6, 0);
	int dropShadowX1 = std::min<int>(x1 + 6, pie_GetVideoBufferWidth());
	int dropShadowY1 = std::min<int>(y1 + 6, pie_GetVideoBufferHeight());
	pie_UniTransBoxFill((float)dropShadowX0, (float)dropShadowY0, (float)dropShadowX1, (float)dropShadowY1, pal_RGBA(0, 0, 0, 40));

	// draw background + border
	pie_UniTransBoxFill((float)x0, (float)y0, (float)x1, (float)y1, pal_RGBA(5, 0, 15, 170));
	iV_Box(x0, y0, x1, y1, pal_RGBA(255, 255, 255, 150));

	// draw title
	int titleXPadding = 10;
	cachedTitleText.setText(_("Hello window lol"), font_regular_bold);

	Vector2i textBoundingBoxOffset(0, 0);
	textBoundingBoxOffset.y = y0 + TITLE_TOP_PADDING + (TITLE_HEIGHT - cachedTitleText.lineSize()) / 2;

	int fw = cachedTitleText.width();
	int fx = x0 + titleXPadding + (width() - titleXPadding - fw) / 2;
	float fy = float(textBoundingBoxOffset.y) - float(cachedTitleText.aboveBase());
	cachedTitleText.render(textBoundingBoxOffset.x + fx, fy, WZCOL_FORM_TEXT);
}

std::shared_ptr<TechTreeThing> TechTreeThing::make() {
	// copypasted straight from WZScriptDebugger::make
	auto result = std::make_shared<TechTreeThing>();

	result->enableMinimizing("TechTreeThing", WZCOL_FORM_TEXT);

	result->id = MULTIOP_OPTIONS;
	int newFormWidth = FRONTEND_TOPFORM_WIDEW;
	result->setGeometry((pie_GetVideoBufferWidth() - newFormWidth) / 2, 10, newFormWidth, pie_GetVideoBufferHeight() - 50);
	result->setCalcLayout(LAMBDA_CALCLAYOUT_SIMPLE({
		// update the height
		int newHeight = std::min<int>(pie_GetVideoBufferHeight() - 50, TECHTREEVIEW_WINDOW_HEIGHT_MAX);
		// update the x if necessary to ensure the entire form is visible
		int x0 = std::min(psWidget->x(), pie_GetVideoBufferWidth() - psWidget->width());
		// update the y if necessary to ensure the entire form is visible
		int y0 = std::min(psWidget->y(), pie_GetVideoBufferHeight() - newHeight);
		psWidget->setGeometry(x0, y0, psWidget->width(), newHeight);
	}));
	result->userMovable = true;

	auto minimizeButton = makeCornerButton("\u21B8"); // ↸
	result->attach(minimizeButton);
	minimizeButton->addOnClickHandler([](W_BUTTON& button){
		auto psParent = std::dynamic_pointer_cast<TechTreeThing>(button.parent());
		ASSERT_OR_RETURN(, psParent != nullptr, "No parent");
		widgScheduleTask([psParent](){
			psParent->toggleMinimized();
		});
	});

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

	result->createButton(40, 40, "Test?", [](){
		if(!lastSelectedLab) {
			debug(LOG_INFO, "Last lab is null");
			return;
		}
		RESEARCH *currSubject = ((RESEARCH_FACILITY *)((STRUCTURE *)lastSelectedLab)->pFunctionality)->psSubject;
		if(!currSubject) {
			debug(LOG_INFO, "subject is null");
			return;
		}
		debug(LOG_INFO, "name: %s", currSubject->name.toUtf8().c_str());
	});

	result->SelectedResearch = makeTechButton();//asResearch[0].psStat);
	result->SelectedResearch->move(120, 30);
	result->attach(result->SelectedResearch);
	result->SelectedResearch->addOnClickHandler([](W_BUTTON& button){
		auto psParent = std::dynamic_pointer_cast<TechTreeThing>(button.parent());
		ASSERT_OR_RETURN(, psParent != nullptr, "No parent");
		debug(LOG_INFO, "Hello world");
	});
	//result->addWidgetToLayout(demothing);

	result->ResultsTable = JSONTableWidget::make("Research products:");
	result->attach(result->ResultsTable);
	result->ResultsTable->setCalcLayout(LAMBDA_CALCLAYOUT_SIMPLE({
		psWidget->setGeometry(10, 80, 300, 300);
	}));
	result->ResultsTable->updateData(result->GetCurrentResearchInfo());

	return result;
}

void TechTreeThing::run(W_CONTEXT *psContext) {
	this->W_FORM::run(psContext);

	if(lastSelectedLab) {
		if(lastSelectedLab->died) {
			lastSelectedLab = nullptr;
		}
	}
	if(this->LastWatchingObject) {
		if(this->LastWatchingObject->died) {
			this->LastWatchingObject = nullptr;
		}
	}

	if(this->LastWatchingObject != lastSelectedLab && !this->lockResearch) {
		this->LastWatchingObject = lastSelectedLab;
		this->SetNewResearchFromLab(this->LastWatchingObject);
	}
	if(this->CurrentResearch != ExtractResearchFromLab(this->LastWatchingObject) && !this->lockResearch) {
		this->SetNewResearch(ExtractResearchFromLab(this->LastWatchingObject));
	}
}

void TechTreeThing::UpdateTableData() {
	this->ResultsTable->updateData(this->GetCurrentResearchInfo());
}
void TechTreeThing::UpdateMainButton() {
	TechButtonData& data = *static_cast<TechButtonData*>(this->SelectedResearch->pUserData);
	if(this->CurrentResearch) {
		data.tech->setStats(this->CurrentResearch);
	} else {
		data.tech->setStats(nullptr);
	}
}
void TechTreeThing::UpdateWidgets() {
	this->UpdateTableData();
	this->UpdateMainButton();
}
void TechTreeThing::SetNewResearch(RESEARCH* newres) {
	this->CurrentResearch = newres;
	this->UpdateWidgets();
}
void TechTreeThing::SetNewResearchFromLab(BASE_OBJECT *psObj) {
	if(!psObj) {
		debug(LOG_INFO, "nullptr on base object");
		this->SetNewResearch(nullptr);
	}
	this->SetNewResearch(ExtractResearchFromLab(psObj));
}
void TechTreeThing::SetNewResearchFromLabAndTrack(BASE_OBJECT *psObj) {
	if(!psObj) {
		debug(LOG_INFO, "nullptr on base object");
		this->SetNewResearch(nullptr);
		return;
	}
	this->SetNewResearchFromLab(psObj);
	this->LastWatchingObject = psObj;
}
void TechTreeThing::TriggerSelectedObject(BASE_OBJECT *psObj) {
	if(!this->lockResearch) {
		this->SetNewResearchFromLabAndTrack(psObj);
	}
}

void treeviewTriggerSelected(BASE_OBJECT *psObj) {
	if(!psObj) {
		debug(LOG_INFO, "nullptr object selected");
		return;
	} else {
		debug(LOG_INFO, "Triggered selection of lab");
	}
	if(((STRUCTURE *)psObj)->pStructureType->type != REF_RESEARCH) {
		debug(LOG_INFO, "selected not lab");
		return;
	}
	lastSelectedLab = psObj;
	if(techviewDialog) {
		techviewDialog->TriggerSelectedObject(psObj);
	}
}

nlohmann::json TechTreeThing::GetCurrentResearchInfo() {
	if(!this->CurrentResearch) {
		return {"no research assigned"};
	}
	nlohmann::json r;
	r["name"] = this->CurrentResearch->name;
	r["cost"] = this->CurrentResearch->researchPower;
	for(auto a : this->CurrentResearch->pPRList) {
		r["requires"].push_back(asResearch[a].name);
	}
	nlohmann::json arr = nlohmann::json::array();
	for(auto i : asResearch) { // walk through all research
		for(auto j : i.pPRList) { // walk through all requirements
			if(asResearch[j].index == this->CurrentResearch->index) {
				arr.push_back(i.name);
			}
		}
	}
	r["opens"] = arr;
	r["results"] = this->CurrentResearch->results;
	return r;
}
static nlohmann::ordered_json fillResearchResults(RESEARCH* current) {
	if(!current) {
		return {"no research assigned"};
	}
	return current->results;
}

static RESEARCH* ExtractResearchFromLab(BASE_OBJECT *psObj) {
	if(!psObj) {
		return nullptr;
	}
	return ((RESEARCH *)((RESEARCH_FACILITY *)((STRUCTURE *)psObj)->pFunctionality)->psSubject);
}
static void TechButtonDisplayFunc(WIDGET *psWidget, UDWORD xOffset, UDWORD yOffset) {
	assert(psWidget->pUserData != nullptr);
	TechButtonData& data = *static_cast<TechButtonData*>(psWidget->pUserData);
	W_BUTTON *psButton = dynamic_cast<W_BUTTON*>(psWidget);
	ASSERT_OR_RETURN(, psButton, "psWidget is null");

	data.tech->state = psButton->getState();
	data.tech->display(psButton->x()+xOffset, psButton->y()+yOffset);
	return;
}
std::shared_ptr<W_BUTTON> makeTechButton() {
	auto button = std::make_shared<W_BUTTON>();
	button->setString("text");
	button->FontID = font_regular_bold;
	button->displayFunction = TechButtonDisplayFunc;

	auto statbtn = std::make_shared<IntStatsButton>();
	statbtn->style |= WFORM_SECONDARY;
	statbtn->setStats(nullptr);

	TechButtonData* d = new TechButtonData();
	d->tech = statbtn;
	button->pUserData = d;
	button->setOnDelete([](WIDGET *psWidget) {
		assert(psWidget->pUserData != nullptr);
		delete static_cast<TechButtonData *>(psWidget->pUserData);
		psWidget->pUserData = nullptr;
	});
	button->setGeometry(STAT_GAP/2, STAT_GAP/2, STAT_BUTWIDTH, STAT_BUTHEIGHT);
	return button;
}

struct CornerButtonDisplayCache {
	WzText text;
};
static void CornerButtonDisplayFunc(WIDGET *psWidget, UDWORD xOffset, UDWORD yOffset) {
	assert(psWidget->pUserData != nullptr);
	CornerButtonDisplayCache& cache = *static_cast<CornerButtonDisplayCache*>(psWidget->pUserData);

	W_BUTTON *psButton = dynamic_cast<W_BUTTON*>(psWidget);
	ASSERT_OR_RETURN(, psButton, "psWidget is null");

	int x0 = psButton->x() + xOffset;
	int y0 = psButton->y() + yOffset;
	int x1 = x0 + psButton->width();
	int y1 = y0 + psButton->height();

	bool haveText = !psButton->pText.isEmpty();

//	bool isDown = (psButton->getState() & (WBUT_DOWN | WBUT_LOCK | WBUT_CLICKLOCK)) != 0;
	bool isDisabled = (psButton->getState() & WBUT_DISABLE) != 0;
	bool isHighlight = (psButton->getState() & WBUT_HIGHLIGHT) != 0;

	if (isHighlight) {
		pie_UniTransBoxFill(x0+1.0f, static_cast<float>(y0 + 1), static_cast<float>(x1 - 1), static_cast<float>(y1 - 1), WZCOL_DEBUG_FILL_COLOR);
		iV_Box(x0 + 1, y0 + 1, x1 - 1, y1 - 1, pal_RGBA(255, 255, 255, 150));
	}

	if (haveText) {
		cache.text.setText(psButton->pText.toUtf8(), psButton->FontID);
		int fw = cache.text.width();
		int fx = x0 + (psButton->width() - fw) / 2;
		int fy = y0 + (psButton->height() - cache.text.lineSize()) / 2 - cache.text.aboveBase();
		PIELIGHT textColor = WZCOL_FORM_TEXT;
		if (isDisabled)
		{
			cache.text.render(fx + 1, fy + 1, WZCOL_FORM_LIGHT);
			textColor = WZCOL_FORM_DISABLE;
		}
		cache.text.render(fx, fy, textColor);
	}
	if (isDisabled) {
		iV_TransBoxFill(x0, y0, x0 + psButton->width(), y0 + psButton->height());
	}
}
static std::shared_ptr<W_BUTTON> makeCornerButton(const char* text) {
	auto button = std::make_shared<W_BUTTON>();
	button->setString(text);
	button->FontID = font_regular_bold;
	button->displayFunction = CornerButtonDisplayFunc;
	button->pUserData = new CornerButtonDisplayCache();
	button->setOnDelete([](WIDGET *psWidget) {
		assert(psWidget->pUserData != nullptr);
		delete static_cast<CornerButtonDisplayCache *>(psWidget->pUserData);
		psWidget->pUserData = nullptr;
	});
	int minButtonWidthForText = iV_GetTextWidth(text, button->FontID);
	int buttonSize = std::max({minButtonWidthForText + TAB_BUTTONS_PADDING, 18, iV_GetTextLineSize(button->FontID)});
	button->setGeometry(0, 0, buttonSize, buttonSize);
	return button;
}

struct TabButtonDisplayCache {
	WzText text;
};
static void TabButtonDisplayFunc(WIDGET *psWidget, UDWORD xOffset, UDWORD yOffset) {
	assert(psWidget->pUserData != nullptr);
	TabButtonDisplayCache& cache = *static_cast<TabButtonDisplayCache*>(psWidget->pUserData);

	W_BUTTON *psButton = dynamic_cast<W_BUTTON*>(psWidget);
	ASSERT_OR_RETURN(, psButton, "psWidget is null");

	int x0 = psButton->x() + xOffset;
	int y0 = psButton->y() + yOffset;
	int x1 = x0 + psButton->width();
	int y1 = y0 + psButton->height();

	bool haveText = !psButton->pText.isEmpty();

	bool isDown = (psButton->getState() & (WBUT_DOWN | WBUT_LOCK | WBUT_CLICKLOCK)) != 0;
	bool isDisabled = (psButton->getState() & WBUT_DISABLE) != 0;
	bool isHighlight = !isDisabled && ((psButton->getState() & WBUT_HIGHLIGHT) != 0);

	auto light_border = WZCOL_DEBUG_BORDER_LIGHT;
	auto fill_color = isDown || isDisabled ? WZCOL_DEBUG_FILL_COLOR_DARK : WZCOL_DEBUG_FILL_COLOR;
	iV_ShadowBox(x0, y0, x1, y1, 0, isDown ? pal_RGBA(0,0,0,0) : light_border, isDisabled ? light_border : WZCOL_FORM_DARK, fill_color);
	if (isHighlight) {
		iV_Box(x0 + 2, y0 + 2, x1 - 2, y1 - 2, WZCOL_FORM_HILITE);
	}
	if (haveText) {
		cache.text.setText(psButton->pText.toUtf8(), psButton->FontID);
		int fw = cache.text.width();
		int fx = x0 + (psButton->width() - fw) / 2;
		int fy = y0 + (psButton->height() - cache.text.lineSize()) / 2 - cache.text.aboveBase();
		PIELIGHT textColor = WZCOL_FORM_TEXT;
		if (isDisabled)
		{
			textColor.byte.a = (textColor.byte.a / 2);
		}
		cache.text.render(fx, fy, textColor);
	}
}

static std::shared_ptr<W_BUTTON> makeDebugButton(const char* text) {
	auto button = std::make_shared<W_BUTTON>();
	button->setString(text);
	button->FontID = font_regular;
	button->displayFunction = TabButtonDisplayFunc;
	button->pUserData = new TabButtonDisplayCache();
	button->setOnDelete([](WIDGET *psWidget) {
		assert(psWidget->pUserData != nullptr);
		delete static_cast<TabButtonDisplayCache *>(psWidget->pUserData);
		psWidget->pUserData = nullptr;
	});
	int minButtonWidthForText = iV_GetTextWidth(text, button->FontID);
	button->setGeometry(0, 0, minButtonWidthForText + TAB_BUTTONS_PADDING, TAB_BUTTONS_HEIGHT);
	return button;
}

std::shared_ptr<W_BUTTON> TechTreeThing::createButton(int offx, int offy, const std::string &text, const std::function<void ()>& onClickFunc) {
	auto button = makeDebugButton(text.c_str());
	button->setGeometry(button->x(), button->y(), button->width() + 10, button->height());
	attach(button);
	button->addOnClickHandler([onClickFunc](W_BUTTON& button) {
		widgScheduleTask([onClickFunc](){
			onClickFunc();
		});
	});
	button->move(offx, offy);
	return button;
}

bool techtreeshutdown() {
	if (techviewScreen) {
		widgRemoveOverlayScreen(techviewScreen);
	}
	if (techviewDialog) {
		techviewDialog->deleteLater();
		techviewDialog = nullptr;
	}
	techviewScreen = nullptr;
	return true;
}

void techtreeshow() {
	if(techviewDialog || techviewDialog) {
		debug(LOG_INFO, "Tried open tech view twice");//%p %p", techviewDialog, techviewScreen);
	}
	techviewScreen = W_SCREEN::make();
	widgRegisterOverlayScreen(techviewScreen, std::numeric_limits<uint16_t>::max() - 3); // lol pastdue said it's alright
	techviewScreen->psForm->hide(); // from wzscriptdebug
	techviewDialog = TechTreeThing::make();
	techviewScreen->psForm->attach(techviewDialog);
}
