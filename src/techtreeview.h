#ifndef TECHTREEVIEW_H_DEFINED
#define TECHTREEVIEW_H_DEFINED

#include "lib/widget/widget.h"
#include "lib/widget/form.h"
#include "lib/widget/jsontable.h"

#include "lib/framework/frame.h"

#include "objectdef.h"

class TechTreeThing : public W_FORM {
public:
	TechTreeThing();
	~TechTreeThing();

	static std::shared_ptr<TechTreeThing> make();

	virtual void display(int xOffset, int yOffset) override;
	virtual void run(W_CONTEXT *psContext) override;

	void SetNewResearch(RESEARCH* newres);
	void SetNewResearchFromLab(BASE_OBJECT *psObj);
	void TrackObject(BASE_OBJECT *psObj);
	void SetNewResearchFromLabAndTrack(BASE_OBJECT *psObj);
	void TriggerSelectedObject(BASE_OBJECT *psObj);

	nlohmann::json GetCurrentResearchInfo();

	void UpdateTableData();
	void UpdateMainButton();
	void UpdateWidgets();

private:
	WzText cachedTitleText;
	std::shared_ptr<W_BUTTON> createButton(int offx, int offy, const std::string &text, const std::function<void ()>& onClickFunc);

	std::shared_ptr<W_BUTTON> SelectedResearch;
	std::vector<std::shared_ptr<W_BUTTON>> Requires;
	std::vector<std::shared_ptr<W_BUTTON>> Unlocks;
	std::shared_ptr<JSONTableWidget> ResultsTable;

	RESEARCH *CurrentResearch = nullptr;
	BASE_OBJECT *LastWatchingObject = nullptr;

	bool lockResearch = false;
};

bool techtreeshutdown();
void techtreeshow();

void treeviewTriggerSelected(BASE_OBJECT *psObj);

#endif /* end of include guard: TECHTREEVIEW_H_DEFINED */
