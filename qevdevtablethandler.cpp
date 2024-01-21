/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qevdevtablethandler_p.h"

#include <QStringList>
#include <QSocketNotifier>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QtCore/private/qcore_unix_p.h>
#include <qpa/qwindowsysteminterface.h>
#ifdef Q_OS_FREEBSD
#include <dev/evdev/input.h>
#else
#include <linux/input.h>
#endif

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(qLcEvdevTablet, "qt.qpa.input")

class QEvdevTabletData
{
public:
    QEvdevTabletData(QEvdevTabletHandler *q_ptr);

    void processInputEvent(input_event *ev);
    void report();

    QEvdevTabletHandler *q;
    int lastEventType;
    QString devName;
    bool down;
};

QEvdevTabletData::QEvdevTabletData(QEvdevTabletHandler *q_ptr)
    : q(q_ptr), lastEventType(0)
{
}

struct key_press {
  Qt::Key key;
  int count;
};

inline double getTimeDelta(struct timeval *current, struct timeval *prev) {
  return (current->tv_sec + current->tv_usec / 1000000.0) -
         (prev->tv_sec + prev->tv_usec / 1000000.0);
}

#define NULL_KEY Qt::Key_unknown // NO-OP (not sent)
#define CLICK_KEY Qt::Key_C // click (count: number of clicks)
#define LONG_CLICK_KEY Qt::Key_L // long click (count: number of clicks)
#define PRESS_ON_KEY Qt::Key_N // press-and-hold on (always sent before long click)
#define PRESS_OFF_KEY Qt::Key_X // press-and-hold off (always sent before long click)
#define PEN_UP_KEY Qt::Key_P // pen lifted from screen (count: number of segments (so far; used for batch undo); also can be used to implement one-off effects)

#define MAX_CYCLE_TIME         0.01 //seconds
#define MAX_SEGSEQ_DELAY       0.4  //seconds
#define MIN_SEG_TIME           0.05 //seconds
#define MAX_CONTACT_CLICK_TIME 0.01 //seconds
#define MAX_CLICK_TIME         0.2  //seconds
#define MAX_DOUBLE_CLICK_TIME  0.4  //seconds

key_press get_trigger(struct input_event *ev_pen) {
  // triggers:
  // Click        || Press&Hold
  // Double Click || DoublePress&Hold
  // Triple Click || TripleClick&Hold
  // Bit 7 encodes trigger on or off
  // Bit 6 encodes Click or Press and hold
  // Bits 0-5 encodes the number ie. (click, double click, press and hold,
  // double press and hold)

  static int            clicks = 0;
  static int            segments = 0;
  static bool           clickRegistered;
  static bool           pressHoldSent = 0;
  static struct timeval prevTime;
  static struct timeval abortTime;
  static bool           abort;
  static struct timeval lastSegmentTime;
  static bool           contact;
  static struct timeval possiblyReleasedTime;
  static bool           possiblyReleased;
  static bool           possiblyLongClick;
  static bool           longClick;
  static struct timeval possiblyLiftedTime;
  static bool           possiblyLifted;
  static bool           possiblyLiftedGotClick;
  static bool           possiblyLiftedGotContact;

  key_press key = {NULL_KEY, 1};

  if (abort) {
    if (!(ev_pen->code == EV_SYN || ev_pen->code == BTN_STYLUS)) { // BTN_TOOL_PEN = 0 is followed by EV_SYN and (possibly) BTN_STYLUS when pulling away pen
      abort = false;

      double elapsedTime = getTimeDelta(&(ev_pen->time), &abortTime);
      if (elapsedTime > MAX_CYCLE_TIME) {
        // the pen moved away from the screen, and has just re-approached; abort and reset state
        // qCDebug(qLcEvdevTablet, "Event: ABORT...");
        clickRegistered = false;
        possiblyLongClick = false;
        longClick = false;
        contact = false;
        possiblyLifted = false;
        possiblyLiftedGotContact = false;
        possiblyLiftedGotClick = false;
        if (pressHoldSent) {
          // pen was pressed and later pulled away... can only know to send hold off after pen re-approaches screen
          qCDebug(qLcEvdevTablet, "Event: PEN PRESS-PULL-AWAY...");
          key.count = clicks;
          key.key = PRESS_OFF_KEY; // press hold off
          pressHoldSent    = false;
        }
        clicks   = 0;
        segments = 0;
      }
    }
  }

  if (ev_pen->code == BTN_TOOL_PEN && ev_pen->value == 0) { // indicates that pen was pulled away from screen, initiate abort sequence
    abortTime = ev_pen->time;
    abort = true;
  }

  if (possiblyLifted) {
    double elapsedTime = getTimeDelta(&(ev_pen->time), &possiblyLiftedTime);

    if (elapsedTime > MAX_CONTACT_CLICK_TIME) {

      qCDebug(qLcEvdevTablet, "Event: PEN LIFT (%d segments in this sequence)...", segments);

      possiblyLiftedGotContact = false;
      possiblyLiftedGotClick = false;
      possiblyLifted = false;

      contact = false;
      key.key = PEN_UP_KEY; // pen-up type message
      key.count = segments;
    }
    else {
      if (ev_pen->code == BTN_STYLUS && ev_pen->value == 1) {
        // qCDebug(qLcEvdevTablet, "got click");
        possiblyLiftedGotClick = true;
      }

      if (ev_pen->code == ABS_DISTANCE && ev_pen->value == 0) {
        // qCDebug(qLcEvdevTablet, "got contact");
        possiblyLiftedGotContact = true;
      }

      // recieving button release and distance 0 codes in rapid succession
      // indicates a button click with the pen on screen, not an actual lift
      if (possiblyLiftedGotClick && possiblyLiftedGotContact) {
        // qCDebug(qLcEvdevTablet, "detected fake lift");

        // abort, this was a fake lift
        possiblyLiftedGotContact = false;
        possiblyLiftedGotClick = false;
        possiblyLifted = false;
      }
    }
  } else if (contact) {
    if (ev_pen->code == ABS_DISTANCE){
      // qCDebug(qLcEvdevTablet, "Possible lift detected");

      // don't register lift immediately to ignore spurious signals
      // when pressing button with pen on screen
      if (!possiblyLifted) possiblyLiftedTime = ev_pen->time;
      possiblyLifted = true;
    }
  } else {
    if (ev_pen->code == ABS_DISTANCE && ev_pen->value == 0) {
      qCDebug(qLcEvdevTablet, "Event: PEN CONTACT...");
      double timeSinceLastSegment = getTimeDelta(&(ev_pen->time), &lastSegmentTime);
      if (timeSinceLastSegment > MAX_SEGSEQ_DELAY) segments = 0; // new segment sequence
      contact = true;
    }
  }


  bool released = false;

  if (ev_pen->code != EV_SYN && possiblyReleased) {
    possiblyReleased = false;

    double elapsedTime = getTimeDelta(&(ev_pen->time), &possiblyReleasedTime);
    if (elapsedTime < MAX_CYCLE_TIME) {
      // if we recieved a non-sync code within a short enough delay, this was a genuine button release (i.e. not a press-and-pull-away)
      released = true;
    }
  }

  if (ev_pen->code == BTN_STYLUS && ev_pen->value == 0) {
    // don't register release immediately to ignore spurious signals
    // when moving pen away from screen with button pressed
    possiblyReleasedTime = ev_pen->time;
    possiblyReleased = true;
  }

  if (pressHoldSent && ev_pen->code == ABS_PRESSURE) possiblyLongClick = false; // abort long click if pen touches screen
  // if (ev_pen->code == ABS_PRESSURE) {
  //   qCDebug(qLcEvdevTablet, "pressure: %d", ev_pen->value);
  // }
  if (ev_pen->code == ABS_PRESSURE && ev_pen->value == 0) {
    // qCDebug(qLcEvdevTablet, "pressure: %d", ev_pen->value);
    segments++; lastSegmentTime = ev_pen->time;
  }

  if (ev_pen->code == BTN_STYLUS && ev_pen->value == 1) {
    if (!contact) {
      prevTime = ev_pen->time; // update most recent time
      clicks += 1;
      // don't set the trigger as we don't have enough
      // info to ascertain the state yet.
      clickRegistered = false;
    } else {
      qCDebug(qLcEvdevTablet, "Event: PEN CONTACT PRESS...");
    }
  }

  if (longClick) {
    qCDebug(qLcEvdevTablet, "Event: PEN LONG CLICK (%d)...", clicks);
    key.count = clicks; // long click type message 0b00xxxxxx
    key.key = LONG_CLICK_KEY; // long click type message 0b00xxxxxx
    clicks = 0;
    longClick = false;
    possiblyLongClick = false;
  }

  if (clicks > 0) {
    double elapsedTime = getTimeDelta(&(ev_pen->time), &prevTime);  // time between presses of button
    if (elapsedTime < MAX_CLICK_TIME) {
      if (released) {
        // qCDebug(qLcEvdevTablet, "Click Detected");
        clickRegistered = true;
      }
    } else if (elapsedTime < MAX_DOUBLE_CLICK_TIME) { // between MCT and MCDT
      if (!clickRegistered) { // button still held or just released
        if (!pressHoldSent) {
          qCDebug(qLcEvdevTablet, "Event: PEN HOLD START (%d)...", clicks);
          key.count = clicks; // press hold on
          key.key = PRESS_ON_KEY;
          pressHoldSent    = true;
          possiblyLongClick = true; // (long click will be aborted if pen touches screen before button release)
        }

        if (pressHoldSent && released) { // edge case: button pressed held and released between MCT and MCDT, pen hold end
          qCDebug(qLcEvdevTablet, "Event: PEN HOLD END (%d)...", clicks);
          key.count = clicks; // press hold off
          key.key = PRESS_OFF_KEY;
          pressHoldSent    = false;
          if (possiblyLongClick) longClick = true; // send a long click in the next cycle
          else clicks = 0;
        }
      }
    } else { // after MDCT
      if (clickRegistered) {
        qCDebug(qLcEvdevTablet, "Event: PEN CLICK (%d)...", clicks);
        key.count = clicks;
        key.key = CLICK_KEY;
        clickRegistered = false;
        clicks        = 0;
      }
      if (released) {
        qCDebug(qLcEvdevTablet, "Event: PEN HOLD END (%d)...", clicks);
        key.count = clicks; // press hold off
        key.key = PRESS_OFF_KEY;
        pressHoldSent    = false;
        if (possiblyLongClick) longClick = true; // send a long click in the next cycle
        else clicks = 0;
      }
    }
  }

  // qCDebug(qLcEvdevTablet, "%x, %d", trigger, clickRegistered);
  return key;
}

void QEvdevTabletData::processInputEvent(input_event *ev)
{
    key_press key = get_trigger(ev);
    if (key.key != NULL_KEY) {
        QWindowSystemInterface::handleKeyEvent(0, QEvent::KeyPress, key.key, Qt::ControlModifier, QString(), false, key.count);
    }
    lastEventType = ev->type;
}

void QEvdevTabletData::report(){
}


QEvdevTabletHandler::QEvdevTabletHandler(const QString &device, const QString &spec, QObject *parent)
    : QObject(parent), m_fd(-1), m_device(device), m_notifier(0), d(0)
{
    Q_UNUSED(spec)

    setObjectName(QLatin1String("Evdev Tablet with Pen Handler mostly for lamy pen"));

    qCDebug(qLcEvdevTablet, "lamy: using %s", qPrintable(device));

    m_fd = QT_OPEN(device.toLocal8Bit().constData(), O_RDONLY | O_NDELAY, 0);
    if (m_fd < 0) {
        qErrnoWarning(errno, "lamy: Cannot open input device %s", qPrintable(device));
        return;
    }

    bool grabSuccess = !ioctl(m_fd, EVIOCGRAB, (void *) 1);
    if (grabSuccess)
        ioctl(m_fd, EVIOCGRAB, (void *) 0);
    else
        qWarning("lamy: %s: The device is grabbed by another process. No events will be read.", qPrintable(device));

    d = new QEvdevTabletData(this);
    if (!queryLimits())
        qWarning("lamy: %s: Unset or invalid ABS limits. Behavior will be unspecified.", qPrintable(device));

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &QEvdevTabletHandler::readData);
}

QEvdevTabletHandler::~QEvdevTabletHandler()
{
    if (m_fd >= 0)
        QT_CLOSE(m_fd);

    delete d;
}

qint64 QEvdevTabletHandler::deviceId() const
{
    return m_fd;
}

bool QEvdevTabletHandler::queryLimits()
{
    bool ok = true;
    char name[128];
    if (ioctl(m_fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
        d->devName = QString::fromLocal8Bit(name);
        qCDebug(qLcEvdevTablet, "lamy: %s: device name: %s", qPrintable(m_device), name);
    }
    return ok;
}

void QEvdevTabletHandler::readData()
{
    input_event buffer[1];
    int n = 0;
    for (; ;) {
        int result = QT_READ(m_fd, reinterpret_cast<char*>(buffer) + n, sizeof(buffer) - n);
        if (!result) {
            qWarning("lamy: %s: Got EOF from input device", qPrintable(m_device));
            return;
        } else if (result < 0) {
            if (errno != EINTR && errno != EAGAIN) {
                qErrnoWarning(errno, "lamy: %s: Could not read from input device", qPrintable(m_device));
                if (errno == ENODEV) { // device got disconnected -> stop reading
                    delete m_notifier;
                    m_notifier = 0;
                    QT_CLOSE(m_fd);
                    m_fd = -1;
                }
                return;
            }
        } else {
            n += result;
            if (n % sizeof(input_event) == 0)
                break;
        }
    }

    n /= sizeof(input_event);

    for (int i = 0; i < n; ++i)
        d->processInputEvent(&buffer[i]);
}


QEvdevTabletHandlerThread::QEvdevTabletHandlerThread(const QString &device, const QString &spec, QObject *parent)
    : QDaemonThread(parent), m_device(device), m_spec(spec), m_handler(0)
{
    start();
}

QEvdevTabletHandlerThread::~QEvdevTabletHandlerThread()
{
    quit();
    wait();
}

void QEvdevTabletHandlerThread::run()
{
    m_handler = new QEvdevTabletHandler(m_device, m_spec);
    exec();
    delete m_handler;
    m_handler = 0;
}


QT_END_NAMESPACE
