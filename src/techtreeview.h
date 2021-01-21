#ifndef TECHTREEVIEW_H_DEFINED
#define TECHTREEVIEW_H_DEFINED

#include "lib/widget/widget.h"
#include "lib/widget/form.h"

#include "lib/framework/frame.h"

class TechTreeThing : public W_FORM {
public:
	TechTreeThing();
	~TechTreeThing();

	static std::shared_ptr<TechTreeThing> make();

	virtual void display(int xOffset, int yOffset) override;
private:
	WzText TechHelloWorldText;
};

bool techtreeshutdown();
void techtreeshow();

#endif /* end of include guard: TECHTREEVIEW_H_DEFINED */
