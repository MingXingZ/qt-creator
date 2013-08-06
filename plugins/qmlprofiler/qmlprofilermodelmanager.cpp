/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "qmlprofilermodelmanager.h"
#include "qmlprofilersimplemodel.h"
#include "qmlprofilerprocessedmodel.h"
#include "qv8profilerdatamodel.h"
#include "qmlprofilertracefile.h"

#include <utils/qtcassert.h>

#include <QDebug>
#include <QFile>

namespace QmlProfiler {
namespace Internal {


/////////////////////////////////////////////////////////////////////
QmlProfilerDataState::QmlProfilerDataState(QmlProfilerModelManager *modelManager, QObject *parent)
    : QObject(parent), m_state(Empty), m_modelManager(modelManager)
{
    connect(this, SIGNAL(error(QString)), m_modelManager, SIGNAL(error(QString)));
    connect(this, SIGNAL(stateChanged()), m_modelManager, SIGNAL(stateChanged()));
}

void QmlProfilerDataState::setState(QmlProfilerDataState::State state)
{
    // It's not an error, we are continuously calling "AcquiringData" for example
    if (m_state == state)
        return;

    switch (state) {
        case Empty:
            // if it's not empty, complain but go on
            QTC_ASSERT(m_modelManager->isEmpty(), /**/);
        break;
        case AcquiringData:
            // we're not supposed to receive new data while processing older data
            QTC_ASSERT(m_state != ProcessingData, return);
        break;
        case ProcessingData:
            QTC_ASSERT(m_state == AcquiringData, return);
        break;
        case Done:
            QTC_ASSERT(m_state == ProcessingData || m_state == Empty, return);
        break;
        default:
            emit error(tr("Trying to set unknown state in events list"));
        break;
    }

    m_state = state;
    emit stateChanged();

    return;
}


/////////////////////////////////////////////////////////////////////
QmlProfilerTraceTime::QmlProfilerTraceTime(QObject *parent) : QObject(parent)
{
    clear();
}

QmlProfilerTraceTime::~QmlProfilerTraceTime()
{
}

qint64 QmlProfilerTraceTime::startTime() const
{
    return m_startTime;
}

qint64 QmlProfilerTraceTime::endTime() const
{
    return m_endTime;
}

qint64 QmlProfilerTraceTime::duration() const
{
    return endTime() - startTime();
}

void QmlProfilerTraceTime::clear()
{
    m_startTime = -1;
    m_endTime = 0;
}

void QmlProfilerTraceTime::setStartTime(qint64 time)
{
    m_startTime = time;
}

void QmlProfilerTraceTime::setEndTime(qint64 time)
{
    m_endTime = time;
}


/////////////////////////////////////////////////////////////////////

class QmlProfilerModelManager::QmlProfilerModelManagerPrivate
{
public:
    QmlProfilerModelManagerPrivate(QmlProfilerModelManager *qq) : q(qq) {}
    ~QmlProfilerModelManagerPrivate() {}
    QmlProfilerModelManager *q;

    QmlProfilerSimpleModel *model;
    QV8ProfilerDataModel *v8Model;
    QmlProfilerDataState *dataState;
    QmlProfilerTraceTime *traceTime;

    QVector <double> partialCounts;
    double progress;
    qint64 estimatedTime;

    // file to load
    QString fileName;
};


QmlProfilerModelManager::QmlProfilerModelManager(Utils::FileInProjectFinder *finder, QObject *parent) :
    QObject(parent), d(new QmlProfilerModelManagerPrivate(this))
{
    d->model = new QmlProfilerProcessedModel(finder, this);
    d->v8Model = new QV8ProfilerDataModel(this);
//    d->model = new QmlProfilerSimpleModel(this);
    d->dataState = new QmlProfilerDataState(this, this);
    d->traceTime = new QmlProfilerTraceTime(this);
}

QmlProfilerModelManager::~QmlProfilerModelManager()
{
    delete d;
}

QmlProfilerTraceTime *QmlProfilerModelManager::traceTime() const
{
    return d->traceTime;
}

QmlProfilerSimpleModel *QmlProfilerModelManager::simpleModel() const
{
    return d->model;
}

QV8ProfilerDataModel *QmlProfilerModelManager::v8Model() const
{
    return d->v8Model;
}

bool QmlProfilerModelManager::isEmpty() const
{
    return d->model->isEmpty() && d->v8Model->isEmpty();
}

int QmlProfilerModelManager::count() const
{
    return d->model->count();
}

double QmlProfilerModelManager::progress() const
{
    return d->progress;
}

int QmlProfilerModelManager::registerModelProxy()
{
    d->partialCounts << 0;
    return d->partialCounts.count()-1;
}

void QmlProfilerModelManager::modelProxyCountUpdated(int proxyId, qint64 count, qint64 max)
{
    d->progress -= d->partialCounts[proxyId] / d->partialCounts.count();

    if (max <= 0)
        d->partialCounts[proxyId] = 1;
    else
        d->partialCounts[proxyId] = (double)count / (double) max;

    d->progress += d->partialCounts[proxyId] / d->partialCounts.count();

    emit progressChanged();
    if (d->progress > 0.99)
        emit dataAvailable();
}

qint64 QmlProfilerModelManager::estimatedProfilingTime() const
{
    return d->estimatedTime;
}

void QmlProfilerModelManager::newTimeEstimation(qint64 estimation)
{
    d->estimatedTime = estimation;
}

void QmlProfilerModelManager::addRangedEvent(int type, int bindingType, qint64 startTime, qint64 length, const QStringList &data, const QmlDebug::QmlEventLocation &location)
{
    // If trace start time was not explicitly set, use the first event
    if (d->traceTime->startTime() == -1)
        d->traceTime->setStartTime(startTime);

    QTC_ASSERT(state() == QmlProfilerDataState::AcquiringData, /**/);
    d->model->addRangedEvent(type, bindingType, startTime, length, data, location);
    emit countChanged();
}

void QmlProfilerModelManager::addV8Event(int depth, const QString &function, const QString &filename,
                                         int lineNumber, double totalTime, double selfTime)
{
    d->v8Model->addV8Event(depth, function, filename, lineNumber,totalTime, selfTime);
}

void QmlProfilerModelManager::addFrameEvent(qint64 time, int framerate, int animationcount)
{
    if (d->traceTime->startTime() == -1)
        d->traceTime->setStartTime(time);

    QTC_ASSERT(state() == QmlProfilerDataState::AcquiringData, /**/);
    d->model->addFrameEvent(time, framerate, animationcount);
    emit countChanged();
}

void QmlProfilerModelManager::addSceneGraphEvent(int eventType, int SGEtype, qint64 startTime, qint64 timing1, qint64 timing2, qint64 timing3, qint64 timing4, qint64 timing5)
{
    if (d->traceTime->startTime() == -1)
        d->traceTime->setStartTime(startTime);

    QTC_ASSERT(state() == QmlProfilerDataState::AcquiringData, /**/);
    d->model->addSceneGraphEvent(eventType, SGEtype, startTime, timing1, timing2, timing3, timing4, timing5);
    emit countChanged();
}

void QmlProfilerModelManager::addPixmapCacheEvent(qint64 time, int pixmapEventType, QString Url, int pixmapWidth, int pixmapHeight, int referenceCount)
{
    if (d->traceTime->startTime() == -1)
        d->traceTime->setStartTime(time);

    QTC_ASSERT(state() == QmlProfilerDataState::AcquiringData, /**/);
    d->model->addPixmapCacheEvent(time, pixmapEventType, Url, pixmapWidth, pixmapHeight, referenceCount);
    emit countChanged();
}


void QmlProfilerModelManager::complete()
{
    if (state() == QmlProfilerDataState::AcquiringData) {
        // If trace end time was not explicitly set, use the last event
        if (d->traceTime->endTime() == 0)
            d->traceTime->setEndTime(d->model->lastTimeMark());
        setState(QmlProfilerDataState::ProcessingData);
        d->model->complete();
        d->v8Model->complete();
        setState(QmlProfilerDataState::Done);
    } else
    if (state() == QmlProfilerDataState::Empty) {
        setState(QmlProfilerDataState::Done);
    } else
    if (state() == QmlProfilerDataState::Done) {
        // repeated Done states are ignored
    } else {
        emit error(tr("Unexpected complete signal in data model"));
    }
}

void QmlProfilerModelManager::save(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly)) {
        emit error(tr("Could not open %1 for writing.").arg(filename));
        return;
    }

    QmlProfilerFileWriter writer;

    writer.setTraceTime(traceTime()->startTime(), traceTime()->endTime(), traceTime()->duration());
    writer.setV8DataModel(d->v8Model);
    writer.setQmlEvents(d->model->getEvents());
    writer.save(&file);
}

void QmlProfilerModelManager::load(const QString &filename)
{
    d->fileName = filename;
    load();
}

void QmlProfilerModelManager::setFilename(const QString &filename)
{
    d->fileName = filename;
}

void QmlProfilerModelManager::load()
{
    QString filename = d->fileName;

    QFile file(filename);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit error(tr("Could not open %1 for reading.").arg(filename));
        return;
    }

    // erase current
    clear();

    setState(QmlProfilerDataState::AcquiringData);

    QmlProfilerFileReader reader;
    connect(&reader, SIGNAL(error(QString)), this, SIGNAL(error(QString)));
    connect(&reader, SIGNAL(rangedEvent(int,int,qint64,qint64,QStringList,QmlDebug::QmlEventLocation)), this,
            SLOT(addRangedEvent(int,int,qint64,qint64,QStringList,QmlDebug::QmlEventLocation)));
    connect(&reader, SIGNAL(traceStartTime(qint64)), traceTime(), SLOT(setStartTime(qint64)));
    connect(&reader, SIGNAL(traceEndTime(qint64)), traceTime(), SLOT(setEndTime(qint64)));
    connect(&reader, SIGNAL(sceneGraphFrame(int,int,qint64,qint64,qint64,qint64,qint64,qint64)),
            this, SLOT(addSceneGraphEvent(int,int,qint64,qint64,qint64,qint64,qint64,qint64)));
    connect(&reader, SIGNAL(pixmapCacheEvent(qint64,int,QString,int,int,int)),
            this, SLOT(addPixmapCacheEvent(qint64,int,QString,int,int,int)));
    connect(&reader, SIGNAL(frame(qint64,int,int)), this, SLOT(addFrameEvent(qint64,int,int)));
    reader.setV8DataModel(d->v8Model);
    reader.load(&file);

    complete();
}


void QmlProfilerModelManager::setState(QmlProfilerDataState::State state)
{
    d->dataState->setState(state);
}

QmlProfilerDataState::State QmlProfilerModelManager::state() const
{
    return d->dataState->state();
}

void QmlProfilerModelManager::clear()
{
    for (int i = 0; i < d->partialCounts.count(); i++)
        d->partialCounts[i] = 0;
    d->progress = 0;
    d->model->clear();
    d->v8Model->clear();
    d->traceTime->clear();

    emit countChanged();
    setState(QmlProfilerDataState::Empty);
}

void QmlProfilerModelManager::prepareForWriting()
{
    setState(QmlProfilerDataState::AcquiringData);
}


}
}
