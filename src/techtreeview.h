#ifndef TECHTREEVIEW_H_DEFINED
#define TECHTREEVIEW_H_DEFINED

#include "lib/widget/widget.h"
#include "lib/widget/form.h"

#include "lib/framework/frame.h"

#include "basedef.h"

class TechTreeThing : public W_FORM {
public:
	TechTreeThing();
	~TechTreeThing();

	static std::shared_ptr<TechTreeThing> make();

	virtual void display(int xOffset, int yOffset) override;
	std::shared_ptr<W_BUTTON> CurrentViewingTechButton;
private:
	std::shared_ptr<W_BUTTON> createButton(int offx, int offy, const std::string &text, const std::function<void ()>& onClickFunc);

	WzText cachedTitleText;

};

bool techtreeshutdown();
void techtreeshow();

void treeviewTriggerSelected(BASE_OBJECT *psObj);

#endif /* end of include guard: TECHTREEVIEW_H_DEFINED */
