#pragma once
// What the operator can ask of the Switcher from the screen: implemented by AppController
// (Task 10), which posts every action to the ProgramLoop; tests use a fake.
#include <QString>

#include "vt/background.h"
#include "vt/switcher.h"
#include "vt/timing.h"

class SwitcherActions {
public:
    virtual ~SwitcherActions() = default;
    virtual void select(vt::Bus, vt::Source) = 0;
    virtual void take() = 0;
    virtual void autoTransition(vt::Direction) = 0;
    virtual void setSpeed(vt::Speed) = 0;
    virtual void setTBar(int position) = 0;              // 0..511
    virtual void setBackground(vt::BkgColour) = 0;
    virtual void loadCrouton(int index, bool run) = 0;   // run = also start Auto (double click)
    virtual void notImplemented(const QString& what) = 0;  // a control that does nothing in v1
};
