/*
 * Copyright 2006 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Stephan Aßmus, superstippi@gmx.de
 */
#ifndef INPUT_TEXT_VIEW_H
#define INPUT_TEXT_VIEW_H


#include <Invoker.h>
#include <TextView.h>


class InputTextView : public BTextView, public BInvoker {
 public:
								InputTextView(BRect frame,
									const char* name,
									BRect textRect,
									uint32 resizingMode,
									uint32 flags);
	virtual						~InputTextView();

	// BTextView interface
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseUp(BPoint where);

	virtual	void				KeyDown(const char* bytes, int32 numBytes);
	virtual	void				MakeFocus(bool focus);

	// BInvoker interface
	virtual	status_t			Invoke(BMessage* message = NULL);

	// InputTextView
	virtual	void				RevertChanges() = 0;
	virtual	void				ApplyChanges() = 0;

 protected:
			bool				fWasFocus;
};

#endif // INPUT_TEXT_VIEW_H
