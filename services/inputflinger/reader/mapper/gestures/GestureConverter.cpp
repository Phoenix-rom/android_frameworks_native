/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gestures/GestureConverter.h"

#include <sstream>

#include <android-base/stringprintf.h>
#include <android/input.h>
#include <ftl/enum.h>
#include <linux/input-event-codes.h>
#include <log/log_main.h>

#include "TouchCursorInputMapperCommon.h"
#include "input/Input.h"

namespace android {

namespace {

uint32_t gesturesButtonToMotionEventButton(uint32_t gesturesButton) {
    switch (gesturesButton) {
        case GESTURES_BUTTON_LEFT:
            return AMOTION_EVENT_BUTTON_PRIMARY;
        case GESTURES_BUTTON_MIDDLE:
            return AMOTION_EVENT_BUTTON_TERTIARY;
        case GESTURES_BUTTON_RIGHT:
            return AMOTION_EVENT_BUTTON_SECONDARY;
        case GESTURES_BUTTON_BACK:
            return AMOTION_EVENT_BUTTON_BACK;
        case GESTURES_BUTTON_FORWARD:
            return AMOTION_EVENT_BUTTON_FORWARD;
        default:
            return 0;
    }
}

} // namespace

GestureConverter::GestureConverter(InputReaderContext& readerContext,
                                   const InputDeviceContext& deviceContext, int32_t deviceId)
      : mDeviceId(deviceId),
        mReaderContext(readerContext),
        mPointerController(readerContext.getPointerController(deviceId)) {
    deviceContext.getAbsoluteAxisInfo(ABS_MT_POSITION_X, &mXAxisInfo);
    deviceContext.getAbsoluteAxisInfo(ABS_MT_POSITION_Y, &mYAxisInfo);
}

std::string GestureConverter::dump() const {
    std::stringstream out;
    out << "Orientation: " << ftl::enum_string(mOrientation) << "\n";
    out << "Axis info:\n";
    out << "  X: " << mXAxisInfo << "\n";
    out << "  Y: " << mYAxisInfo << "\n";
    out << StringPrintf("Button state: 0x%08x\n", mButtonState);
    out << "Down time: " << mDownTime << "\n";
    out << "Current classification: " << ftl::enum_string(mCurrentClassification) << "\n";
    return out.str();
}

void GestureConverter::reset() {
    mButtonState = 0;
}

std::list<NotifyArgs> GestureConverter::handleGesture(nsecs_t when, nsecs_t readTime,
                                                      const Gesture& gesture) {
    switch (gesture.type) {
        case kGestureTypeMove:
            return {handleMove(when, readTime, gesture)};
        case kGestureTypeButtonsChange:
            return handleButtonsChange(when, readTime, gesture);
        case kGestureTypeScroll:
            return handleScroll(when, readTime, gesture);
        case kGestureTypeFling:
            return {handleFling(when, readTime, gesture)};
        case kGestureTypeSwipe:
            return handleMultiFingerSwipe(when, readTime, 3, gesture.details.swipe.dx,
                                          gesture.details.swipe.dy);
        case kGestureTypeFourFingerSwipe:
            return handleMultiFingerSwipe(when, readTime, 4, gesture.details.four_finger_swipe.dx,
                                          gesture.details.four_finger_swipe.dy);
        case kGestureTypeSwipeLift:
        case kGestureTypeFourFingerSwipeLift:
            return handleMultiFingerSwipeLift(when, readTime);
        case kGestureTypePinch:
            return handlePinch(when, readTime, gesture);
        default:
            // TODO(b/251196347): handle more gesture types.
            return {};
    }
}

NotifyArgs GestureConverter::handleMove(nsecs_t when, nsecs_t readTime, const Gesture& gesture) {
    float deltaX = gesture.details.move.dx;
    float deltaY = gesture.details.move.dy;
    rotateDelta(mOrientation, &deltaX, &deltaY);

    mPointerController->setPresentation(PointerControllerInterface::Presentation::POINTER);
    mPointerController->move(deltaX, deltaY);
    mPointerController->unfade(PointerControllerInterface::Transition::IMMEDIATE);
    float xCursorPosition, yCursorPosition;
    mPointerController->getPosition(&xCursorPosition, &yCursorPosition);

    PointerCoords coords;
    coords.clear();
    coords.setAxisValue(AMOTION_EVENT_AXIS_X, xCursorPosition);
    coords.setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
    coords.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_X, deltaX);
    coords.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_Y, deltaY);
    const bool down = isPointerDown(mButtonState);
    coords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, down ? 1.0f : 0.0f);

    const int32_t action = down ? AMOTION_EVENT_ACTION_MOVE : AMOTION_EVENT_ACTION_HOVER_MOVE;
    return makeMotionArgs(when, readTime, action, /* actionButton= */ 0, mButtonState,
                          /* pointerCount= */ 1, mFingerProps.data(), &coords, xCursorPosition,
                          yCursorPosition);
}

std::list<NotifyArgs> GestureConverter::handleButtonsChange(nsecs_t when, nsecs_t readTime,
                                                            const Gesture& gesture) {
    std::list<NotifyArgs> out = {};

    mPointerController->setPresentation(PointerControllerInterface::Presentation::POINTER);
    mPointerController->unfade(PointerControllerInterface::Transition::IMMEDIATE);

    float xCursorPosition, yCursorPosition;
    mPointerController->getPosition(&xCursorPosition, &yCursorPosition);

    PointerCoords coords;
    coords.clear();
    coords.setAxisValue(AMOTION_EVENT_AXIS_X, xCursorPosition);
    coords.setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
    coords.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_X, 0);
    coords.setAxisValue(AMOTION_EVENT_AXIS_RELATIVE_Y, 0);
    const uint32_t buttonsPressed = gesture.details.buttons.down;
    bool pointerDown = isPointerDown(mButtonState) ||
            buttonsPressed &
                    (GESTURES_BUTTON_LEFT | GESTURES_BUTTON_MIDDLE | GESTURES_BUTTON_RIGHT);
    coords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, pointerDown ? 1.0f : 0.0f);

    uint32_t newButtonState = mButtonState;
    std::list<NotifyArgs> pressEvents = {};
    for (uint32_t button = 1; button <= GESTURES_BUTTON_FORWARD; button <<= 1) {
        if (buttonsPressed & button) {
            uint32_t actionButton = gesturesButtonToMotionEventButton(button);
            newButtonState |= actionButton;
            pressEvents.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_BUTTON_PRESS,
                                                 actionButton, newButtonState,
                                                 /* pointerCount= */ 1, mFingerProps.data(),
                                                 &coords, xCursorPosition, yCursorPosition));
        }
    }
    if (!isPointerDown(mButtonState) && isPointerDown(newButtonState)) {
        mDownTime = when;
        out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_DOWN,
                                     /* actionButton= */ 0, newButtonState, /* pointerCount= */ 1,
                                     mFingerProps.data(), &coords, xCursorPosition,
                                     yCursorPosition));
    }
    out.splice(out.end(), pressEvents);

    // The same button may be in both down and up in the same gesture, in which case we should treat
    // it as having gone down and then up. So, we treat a single button change gesture as two state
    // changes: a set of buttons going down, followed by a set of buttons going up.
    mButtonState = newButtonState;

    const uint32_t buttonsReleased = gesture.details.buttons.up;
    for (uint32_t button = 1; button <= GESTURES_BUTTON_FORWARD; button <<= 1) {
        if (buttonsReleased & button) {
            uint32_t actionButton = gesturesButtonToMotionEventButton(button);
            newButtonState &= ~actionButton;
            out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_BUTTON_RELEASE,
                                         actionButton, newButtonState, /* pointerCount= */ 1,
                                         mFingerProps.data(), &coords, xCursorPosition,
                                         yCursorPosition));
        }
    }
    if (isPointerDown(mButtonState) && !isPointerDown(newButtonState)) {
        coords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 0.0f);
        out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_UP, /* actionButton= */ 0,
                                     newButtonState, /* pointerCount= */ 1, mFingerProps.data(),
                                     &coords, xCursorPosition, yCursorPosition));
    }
    mButtonState = newButtonState;
    return out;
}

std::list<NotifyArgs> GestureConverter::handleScroll(nsecs_t when, nsecs_t readTime,
                                                     const Gesture& gesture) {
    std::list<NotifyArgs> out;
    PointerCoords& coords = mFakeFingerCoords[0];
    float xCursorPosition, yCursorPosition;
    mPointerController->getPosition(&xCursorPosition, &yCursorPosition);
    if (mCurrentClassification != MotionClassification::TWO_FINGER_SWIPE) {
        mCurrentClassification = MotionClassification::TWO_FINGER_SWIPE;
        coords.setAxisValue(AMOTION_EVENT_AXIS_X, xCursorPosition);
        coords.setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
        coords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 1.0f);
        mDownTime = when;
        NotifyMotionArgs args =
                makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_DOWN, /* actionButton= */ 0,
                               mButtonState, /* pointerCount= */ 1, mFingerProps.data(),
                               mFakeFingerCoords.data(), xCursorPosition, yCursorPosition);
        args.flags |= AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE;
        out.push_back(args);
    }
    float deltaX = gesture.details.scroll.dx;
    float deltaY = gesture.details.scroll.dy;
    rotateDelta(mOrientation, &deltaX, &deltaY);

    coords.setAxisValue(AMOTION_EVENT_AXIS_X, coords.getAxisValue(AMOTION_EVENT_AXIS_X) + deltaX);
    coords.setAxisValue(AMOTION_EVENT_AXIS_Y, coords.getAxisValue(AMOTION_EVENT_AXIS_Y) + deltaY);
    // TODO(b/262876643): set AXIS_GESTURE_{X,Y}_OFFSET.
    coords.setAxisValue(AMOTION_EVENT_AXIS_GESTURE_SCROLL_X_DISTANCE, -gesture.details.scroll.dx);
    coords.setAxisValue(AMOTION_EVENT_AXIS_GESTURE_SCROLL_Y_DISTANCE, -gesture.details.scroll.dy);
    NotifyMotionArgs args =
            makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_MOVE, /* actionButton= */ 0,
                           mButtonState, /* pointerCount= */ 1, mFingerProps.data(),
                           mFakeFingerCoords.data(), xCursorPosition, yCursorPosition);
    args.flags |= AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE;
    out.push_back(args);
    return out;
}

NotifyArgs GestureConverter::handleFling(nsecs_t when, nsecs_t readTime, const Gesture& gesture) {
    // We don't actually want to use the gestures library's fling velocity values (to ensure
    // consistency between touchscreen and touchpad flings), so we're just using the "start fling"
    // gestures as a marker for the end of a two-finger scroll gesture.
    if (gesture.details.fling.fling_state != GESTURES_FLING_START ||
        mCurrentClassification != MotionClassification::TWO_FINGER_SWIPE) {
        return {};
    }

    float xCursorPosition, yCursorPosition;
    mPointerController->getPosition(&xCursorPosition, &yCursorPosition);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_SCROLL_X_DISTANCE, 0);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_SCROLL_Y_DISTANCE, 0);
    NotifyMotionArgs args =
            makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_UP, /* actionButton= */ 0,
                           mButtonState, /* pointerCount= */ 1, mFingerProps.data(),
                           mFakeFingerCoords.data(), xCursorPosition, yCursorPosition);
    args.flags |= AMOTION_EVENT_FLAG_IS_GENERATED_GESTURE;
    mCurrentClassification = MotionClassification::NONE;
    return args;
}

[[nodiscard]] std::list<NotifyArgs> GestureConverter::handleMultiFingerSwipe(nsecs_t when,
                                                                             nsecs_t readTime,
                                                                             uint32_t fingerCount,
                                                                             float dx, float dy) {
    std::list<NotifyArgs> out = {};
    float xCursorPosition, yCursorPosition;
    mPointerController->getPosition(&xCursorPosition, &yCursorPosition);
    if (mCurrentClassification != MotionClassification::MULTI_FINGER_SWIPE) {
        // If the user changes the number of fingers mid-way through a swipe (e.g. they start with
        // three and then put a fourth finger down), the gesture library will treat it as two
        // separate swipes with an appropriate lift event between them, so we don't have to worry
        // about the finger count changing mid-swipe.
        mCurrentClassification = MotionClassification::MULTI_FINGER_SWIPE;
        mSwipeFingerCount = fingerCount;

        constexpr float FAKE_FINGER_SPACING = 100;
        float xCoord = xCursorPosition - FAKE_FINGER_SPACING * (mSwipeFingerCount - 1) / 2;
        for (size_t i = 0; i < mSwipeFingerCount; i++) {
            PointerCoords& coords = mFakeFingerCoords[i];
            coords.clear();
            coords.setAxisValue(AMOTION_EVENT_AXIS_X, xCoord);
            coords.setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
            coords.setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 1.0f);
            xCoord += FAKE_FINGER_SPACING;
        }

        mDownTime = when;
        out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_DOWN,
                                     /* actionButton= */ 0, mButtonState, /* pointerCount= */ 1,
                                     mFingerProps.data(), mFakeFingerCoords.data(), xCursorPosition,
                                     yCursorPosition));
        for (size_t i = 1; i < mSwipeFingerCount; i++) {
            out.push_back(makeMotionArgs(when, readTime,
                                         AMOTION_EVENT_ACTION_POINTER_DOWN |
                                                 (i << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                         /* actionButton= */ 0, mButtonState,
                                         /* pointerCount= */ i + 1, mFingerProps.data(),
                                         mFakeFingerCoords.data(), xCursorPosition,
                                         yCursorPosition));
        }
    }
    // TODO(b/251196347): Set the gesture properties appropriately to avoid needing to negate the Y
    // values.
    float rotatedDeltaX = dx, rotatedDeltaY = -dy;
    rotateDelta(mOrientation, &rotatedDeltaX, &rotatedDeltaY);
    for (size_t i = 0; i < mSwipeFingerCount; i++) {
        PointerCoords& coords = mFakeFingerCoords[i];
        coords.setAxisValue(AMOTION_EVENT_AXIS_X,
                            coords.getAxisValue(AMOTION_EVENT_AXIS_X) + rotatedDeltaX);
        coords.setAxisValue(AMOTION_EVENT_AXIS_Y,
                            coords.getAxisValue(AMOTION_EVENT_AXIS_Y) + rotatedDeltaY);
    }
    float xOffset = dx / (mXAxisInfo.maxValue - mXAxisInfo.minValue);
    // TODO(b/251196347): Set the gesture properties appropriately to avoid needing to negate the Y
    // values.
    float yOffset = -dy / (mYAxisInfo.maxValue - mYAxisInfo.minValue);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_X_OFFSET, xOffset);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_Y_OFFSET, yOffset);
    out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_MOVE, /* actionButton= */ 0,
                                 mButtonState, /* pointerCount= */ mSwipeFingerCount,
                                 mFingerProps.data(), mFakeFingerCoords.data(), xCursorPosition,
                                 yCursorPosition));
    return out;
}

[[nodiscard]] std::list<NotifyArgs> GestureConverter::handleMultiFingerSwipeLift(nsecs_t when,
                                                                                 nsecs_t readTime) {
    std::list<NotifyArgs> out = {};
    if (mCurrentClassification != MotionClassification::MULTI_FINGER_SWIPE) {
        return out;
    }
    float xCursorPosition, yCursorPosition;
    mPointerController->getPosition(&xCursorPosition, &yCursorPosition);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_X_OFFSET, 0);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_Y_OFFSET, 0);

    for (size_t i = mSwipeFingerCount; i > 1; i--) {
        out.push_back(makeMotionArgs(when, readTime,
                                     AMOTION_EVENT_ACTION_POINTER_UP |
                                             ((i - 1) << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT),
                                     /* actionButton= */ 0, mButtonState, /* pointerCount= */ i,
                                     mFingerProps.data(), mFakeFingerCoords.data(), xCursorPosition,
                                     yCursorPosition));
    }
    out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_UP,
                                 /* actionButton= */ 0, mButtonState, /* pointerCount= */ 1,
                                 mFingerProps.data(), mFakeFingerCoords.data(), xCursorPosition,
                                 yCursorPosition));
    mCurrentClassification = MotionClassification::NONE;
    mSwipeFingerCount = 0;
    return out;
}

[[nodiscard]] std::list<NotifyArgs> GestureConverter::handlePinch(nsecs_t when, nsecs_t readTime,
                                                                  const Gesture& gesture) {
    std::list<NotifyArgs> out;
    float xCursorPosition, yCursorPosition;
    mPointerController->getPosition(&xCursorPosition, &yCursorPosition);

    // Pinch gesture phases are reported a little differently from others, in that the same details
    // struct is used for all phases of the gesture, just with different zoom_state values. When
    // zoom_state is START or END, dz will always be 1, so we don't need to move the pointers in
    // those cases.

    if (mCurrentClassification != MotionClassification::PINCH) {
        LOG_ALWAYS_FATAL_IF(gesture.details.pinch.zoom_state != GESTURES_ZOOM_START,
                            "First pinch gesture does not have the START zoom state (%d instead).",
                            gesture.details.pinch.zoom_state);
        mCurrentClassification = MotionClassification::PINCH;
        mPinchFingerSeparation = INITIAL_PINCH_SEPARATION_PX;
        mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR, 1.0);
        mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_X,
                                          xCursorPosition - mPinchFingerSeparation / 2);
        mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
        mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 1.0f);
        mFakeFingerCoords[1].setAxisValue(AMOTION_EVENT_AXIS_X,
                                          xCursorPosition + mPinchFingerSeparation / 2);
        mFakeFingerCoords[1].setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
        mFakeFingerCoords[1].setAxisValue(AMOTION_EVENT_AXIS_PRESSURE, 1.0f);
        mDownTime = when;
        out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_DOWN,
                                     /* actionButton= */ 0, mButtonState, /* pointerCount= */ 1,
                                     mFingerProps.data(), mFakeFingerCoords.data(), xCursorPosition,
                                     yCursorPosition));
        out.push_back(makeMotionArgs(when, readTime,
                                     AMOTION_EVENT_ACTION_POINTER_DOWN |
                                             1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT,
                                     /* actionButton= */ 0, mButtonState, /* pointerCount= */ 2,
                                     mFingerProps.data(), mFakeFingerCoords.data(), xCursorPosition,
                                     yCursorPosition));
        return out;
    }

    if (gesture.details.pinch.zoom_state == GESTURES_ZOOM_END) {
        mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR, 1.0);
        out.push_back(makeMotionArgs(when, readTime,
                                     AMOTION_EVENT_ACTION_POINTER_UP |
                                             1 << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT,
                                     /* actionButton= */ 0, mButtonState, /* pointerCount= */ 2,
                                     mFingerProps.data(), mFakeFingerCoords.data(), xCursorPosition,
                                     yCursorPosition));
        out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_UP, /* actionButton= */ 0,
                                     mButtonState, /* pointerCount= */ 1, mFingerProps.data(),
                                     mFakeFingerCoords.data(), xCursorPosition, yCursorPosition));
        mCurrentClassification = MotionClassification::NONE;
        mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR, 0);
        return out;
    }

    mPinchFingerSeparation *= gesture.details.pinch.dz;
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_GESTURE_PINCH_SCALE_FACTOR,
                                      gesture.details.pinch.dz);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_X,
                                      xCursorPosition - mPinchFingerSeparation / 2);
    mFakeFingerCoords[0].setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
    mFakeFingerCoords[1].setAxisValue(AMOTION_EVENT_AXIS_X,
                                      xCursorPosition + mPinchFingerSeparation / 2);
    mFakeFingerCoords[1].setAxisValue(AMOTION_EVENT_AXIS_Y, yCursorPosition);
    out.push_back(makeMotionArgs(when, readTime, AMOTION_EVENT_ACTION_MOVE, /* actionButton= */ 0,
                                 mButtonState, /* pointerCount= */ 2, mFingerProps.data(),
                                 mFakeFingerCoords.data(), xCursorPosition, yCursorPosition));
    return out;
}

NotifyMotionArgs GestureConverter::makeMotionArgs(nsecs_t when, nsecs_t readTime, int32_t action,
                                                  int32_t actionButton, int32_t buttonState,
                                                  uint32_t pointerCount,
                                                  const PointerProperties* pointerProperties,
                                                  const PointerCoords* pointerCoords,
                                                  float xCursorPosition, float yCursorPosition) {
    // TODO(b/260226362): consider what the appropriate source for these events is.
    const uint32_t source = AINPUT_SOURCE_MOUSE;

    return NotifyMotionArgs(mReaderContext.getNextId(), when, readTime, mDeviceId, source,
                            mPointerController->getDisplayId(), /* policyFlags= */ POLICY_FLAG_WAKE,
                            action, /* actionButton= */ actionButton, /* flags= */ 0,
                            mReaderContext.getGlobalMetaState(), buttonState,
                            mCurrentClassification, AMOTION_EVENT_EDGE_FLAG_NONE, pointerCount,
                            pointerProperties, pointerCoords, /* xPrecision= */ 1.0f,
                            /* yPrecision= */ 1.0f, xCursorPosition, yCursorPosition,
                            /* downTime= */ mDownTime, /* videoFrames= */ {});
}

} // namespace android
