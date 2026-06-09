// core/IModule.cpp
#include "IModule.h"

#include <QQmlApplicationEngine>
#include <QQmlContext>

void IModule::registerWithQml(QQmlApplicationEngine *engine)
{
    if (qmlObject() && !qmlName().isEmpty())
        engine->rootContext()->setContextProperty(qmlName(), qmlObject());
}
