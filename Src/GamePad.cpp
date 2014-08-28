//--------------------------------------------------------------------------------------
// File: GamePad.cpp
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkId=248929
//--------------------------------------------------------------------------------------

#include "pch.h"

#include "GamePad.h"
#include "PlatformHelpers.h"

using namespace DirectX;

namespace
{
    float ApplyLinearDeadZone( float value, float maxValue, float deadZoneSize )
    {
        if ( value < -deadZoneSize )
        {
            // Increase negative values to remove the deadzone discontinuity.
            value += deadZoneSize;
        }
        else if ( value > deadZoneSize )
        {
            // Decrease positive values to remove the deadzone discontinuity.
            value -= deadZoneSize;
        }
        else
        {
            // Values inside the deadzone come out zero.
            return 0;
        }

        // Scale into 0-1 range.
        float scaledValue = value / (maxValue - deadZoneSize);
        return std::max( -1.f, std::min( scaledValue, 1.f ) );
    }

    void ApplyStickDeadZone( float x, float y, GamePad::DeadZone deadZoneMode, float maxValue, float deadZoneSize,
                             _Out_ float& resultX, _Out_ float& resultY)
    {
        switch( deadZoneMode )
        {
        case GamePad::DEAD_ZONE_INDEPENDENT_AXES:
            resultX = ApplyLinearDeadZone( x, maxValue, deadZoneSize );
            resultY = ApplyLinearDeadZone( y, maxValue, deadZoneSize );
            break;

        case GamePad::DEAD_ZONE_CIRCULAR:
            {
                float dist = sqrtf( x*x + y*y );
                float wanted = ApplyLinearDeadZone( dist, maxValue, deadZoneSize );

                float scale = (wanted > 0.f) ? ( wanted / dist ) : 0.f;

                resultX = std::max( -1.f, std::min( x * scale, 1.f ) );
                resultY = std::max( -1.f, std::min( y * scale, 1.f ) );
            }
            break;

        default: // GamePad::DEAD_ZONE_NONE
            resultX = ApplyLinearDeadZone( x, maxValue, 0 );
            resultY = ApplyLinearDeadZone( y, maxValue, 0 );
            break;
        }
    }
}


#ifdef _XBOX_ONE

//======================================================================================
// Windows::Xbox::Input
//======================================================================================

#include <Windows.Xbox.Input.h>

#ifdef _TITLE
#include <Windows.Foundation.Collections.h>

namespace
{

class GamepadAddedListener : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                                 ABI::Windows::Foundation::IEventHandler<ABI::Windows::Xbox::Input::GamepadAddedEventArgs *>,
                                                                 Microsoft::WRL::FtmBase>
{
public:
    GamepadAddedListener(HANDLE event) : mEvent(event) {}

    STDMETHOD(Invoke)(_In_ IInspectable *, _In_ ABI::Windows::Xbox::Input::IGamepadAddedEventArgs * ) override
    {
        SetEvent( mEvent );
        return S_OK;
    }

private:
    HANDLE mEvent;
};

class GamepadRemovedListener : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                                                   ABI::Windows::Foundation::IEventHandler<ABI::Windows::Xbox::Input::GamepadRemovedEventArgs *>,
                                                                   Microsoft::WRL::FtmBase>
{
public:
    GamepadRemovedListener(HANDLE event) : mEvent(event) {}

    STDMETHOD(Invoke)(_In_ IInspectable *, _In_ ABI::Windows::Xbox::Input::IGamepadRemovedEventArgs * ) override
    {
        SetEvent( mEvent );
        return S_OK;
    }

private:
    HANDLE mEvent;
};

}
#endif

class GamePad::Impl
{
public:
    Impl()
    {
        using namespace Microsoft::WRL;
        using namespace Microsoft::WRL::Wrappers;
        using namespace ABI::Windows::Foundation;
        
        mAddedToken.value = 0;
        mRemovedToken.value = 0;

        if ( s_changed )
        {
            throw std::exception( "GamePad is a singleton" );
        }

        s_changed = CreateEventEx( nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE );
        if ( !s_changed )
        {
            throw std::exception( "CreateEventEx" );
        }

        HRESULT hr = GetActivationFactory( HStringReference(RuntimeClass_Windows_Xbox_Input_Gamepad).Get(), mStatics.GetAddressOf() );
        ThrowIfFailed( hr );

#ifdef _TITLE
        // This is a workaround for some registration issues in the GameOS

        hr = mStatics->add_GamepadAdded(Make<GamepadAddedListener>(s_changed).Get(), &mAddedToken );
        ThrowIfFailed( hr );

        hr = mStatics->add_GamepadRemoved(Make<GamepadRemovedListener>(s_changed).Get(), &mRemovedToken );
        ThrowIfFailed( hr );
#else
        typedef __FIEventHandler_1_Windows__CXbox__CInput__CGamepadAddedEventArgs AddedHandler;
        hr = mStatics->add_GamepadAdded(Callback<AddedHandler>(GamepadAdded).Get(), &mAddedToken );
        ThrowIfFailed( hr );

        typedef __FIEventHandler_1_Windows__CXbox__CInput__CGamepadRemovedEventArgs RemovedHandler;
        hr = mStatics->add_GamepadRemoved(Callback<RemovedHandler>(GamepadRemoved).Get(), &mRemovedToken );
        ThrowIfFailed( hr );
#endif

        ScanGamePads();
    }

    ~Impl()
    {
        if ( mStatics )
        {
            mStatics->remove_GamepadAdded( mAddedToken );
            mStatics->remove_GamepadRemoved( mRemovedToken );

            mStatics.Reset();
        }

        if ( s_changed )
        {
            CloseHandle( s_changed );
            s_changed = nullptr;
        }
    }

    void GetState( int player, _Out_ State& state, DeadZone deadZoneMode )
    {
        using namespace Microsoft::WRL;
        using namespace ABI::Windows::Xbox::Input;

        if ( WaitForSingleObjectEx( s_changed, 0, FALSE ) == WAIT_OBJECT_0 )
        {
            ScanGamePads();
        }

        if ( ( player >= 0 ) && ( player < MAX_PLAYER_COUNT ) )
        {
            if ( mGamePad[ player ] )
            {
                ComPtr<IGamepadReading> reading;
                HRESULT hr = mGamePad[ player ]->GetCurrentReading( reading.GetAddressOf() );
                if ( SUCCEEDED(hr) )
                {
                    state.connected = true;

                    GamepadButtons buttons;
                    hr = reading->get_Buttons( &buttons );
                    if ( FAILED(hr) )
                        buttons = GamepadButtons::GamepadButtons_None;

                    state.buttons.a  = (buttons & GamepadButtons::GamepadButtons_A) != 0;
                    state.buttons.b  = (buttons & GamepadButtons::GamepadButtons_B) != 0;
                    state.buttons.x  = (buttons & GamepadButtons::GamepadButtons_X) != 0;
                    state.buttons.y  = (buttons & GamepadButtons::GamepadButtons_Y) != 0;

                    state.buttons.leftStick = (buttons & GamepadButtons::GamepadButtons_LeftThumbstick) != 0;
                    state.buttons.rightStick = (buttons & GamepadButtons::GamepadButtons_RightThumbstick) != 0;

                    state.buttons.leftShoulder = (buttons & GamepadButtons::GamepadButtons_LeftShoulder) != 0;
                    state.buttons.rightShoulder = (buttons & GamepadButtons::GamepadButtons_RightShoulder) != 0;

                    state.buttons.back = (buttons & GamepadButtons::GamepadButtons_View) != 0;
                    state.buttons.start = (buttons & GamepadButtons::GamepadButtons_Menu) != 0;

                    state.dpad.up = (buttons & GamepadButtons::GamepadButtons_DPadUp) != 0;
                    state.dpad.down = (buttons & GamepadButtons::GamepadButtons_DPadDown) != 0;
                    state.dpad.right = (buttons & GamepadButtons::GamepadButtons_DPadRight) != 0;
                    state.dpad.left = (buttons & GamepadButtons::GamepadButtons_DPadLeft) != 0;

                    float stickX;
                    hr = reading->get_LeftThumbstickX( &stickX );
                    if ( FAILED(hr) )
                        stickX = 0.f;

                    float stickY;
                    hr = reading->get_LeftThumbstickY( &stickY );
                    if ( FAILED(hr) )
                        stickY = 0.f;

                    ApplyStickDeadZone( stickX, stickY,
                                        deadZoneMode, 1.f, .24f /* Recommend Xbox One deadzone */,
                                        state.thumbSticks.leftX, state.thumbSticks.leftY );

                    hr = reading->get_RightThumbstickX( &stickX );
                    if ( FAILED(hr) )
                        stickX = 0.f;

                    hr = reading->get_RightThumbstickY( &stickY );
                    if ( FAILED(hr) )
                        stickY = 0.f;

                    ApplyStickDeadZone( stickX, stickY,
                                        deadZoneMode, 1.f, .24f /* Recommend Xbox One deadzone */,
                                        state.thumbSticks.rightX, state.thumbSticks.rightY );

                    hr = reading->get_LeftTrigger( &state.triggers.left );
                    if ( FAILED(hr) )
                        state.triggers.left = 0.f;

                    hr = reading->get_RightTrigger( &state.triggers.right );
                    if ( FAILED(hr) )
                        state.triggers.right = 0.f;
                     
                    return;
                }
            }
        }

        memset( &state, 0, sizeof(State) );
    }

    void GetCapabilities( int player, _Out_ Capabilities& caps )
    {
        if ( WaitForSingleObjectEx( s_changed, 0, FALSE ) == WAIT_OBJECT_0 )
        {
            ScanGamePads();
        }

        if ( ( player >= 0 ) && ( player < MAX_PLAYER_COUNT ) )
        {
            if ( mGamePad[ player ] )
            {
                caps.connected = true;
                caps.gamepadType = Capabilities::GAMEPAD;

                Microsoft::WRL::ComPtr<ABI::Windows::Xbox::Input::IController> ctrl;
                HRESULT hr = mGamePad[ player ].As( &ctrl );
                if ( SUCCEEDED(hr) && ctrl )
                {
                    hr = ctrl->get_Id( &caps.id );
                    if ( FAILED(hr) )
                        caps.id = 0;
                }
                else
                    caps.id = 0;
                
                return;
            }
        }

        memset( &caps, 0, sizeof(Capabilities) );
    }

    bool SetVibration( int player, float leftMotor, float rightMotor )
    {
        using namespace ABI::Windows::Xbox::Input;

        if ( ( player >= 0 ) && ( player < MAX_PLAYER_COUNT ) )
        {
            if ( mGamePad[ player ] )
            {
                HRESULT hr;
                try
                {
                    GamepadVibration vib;
                    vib.LeftMotorLevel = leftMotor;
                    vib.LeftTriggerLevel = 0.f;
                    vib.RightMotorLevel = rightMotor;
                    vib.RightTriggerLevel = 0.f;
                    hr = mGamePad[ player ]->SetVibration(vib);
                }
                catch( ... )
                {
                    // Handle case where gamepad might be invalid
                    hr = E_FAIL;
                }

                if ( SUCCEEDED(hr) )
                    return true;
            }
        }

        return false;
    }

    void Suspend()
    {
        for( size_t j = 0; j < MAX_PLAYER_COUNT; ++j )
        {
            mGamePad[ j ].Reset();
        }
    }

    void Resume()
    {
        // Make sure we rescan gamepads
        SetEvent( s_changed );
    }

private:
    void ScanGamePads()
    {
        using namespace Microsoft::WRL;
        using namespace ABI::Windows::Foundation::Collections;
        using namespace ABI::Windows::Xbox::Input;

        ComPtr<IVectorView<IGamepad*>> pads;
        HRESULT hr = mStatics->get_Gamepads( pads.GetAddressOf() );
        ThrowIfFailed( hr );

        unsigned int count = 0;
        pads->get_Size( &count );
        ThrowIfFailed( hr );

        // Check for removed gamepads
        for( size_t j = 0; j < MAX_PLAYER_COUNT; ++j )
        {
            if ( mGamePad[ j ] )
            {
                unsigned int k = 0;
                for( ; k < count; ++k )
                {
                    ComPtr<IGamepad> pad;
                    hr = pads->GetAt( k, pad.GetAddressOf() );
                    if ( SUCCEEDED(hr) && ( pad == mGamePad[ j ] ) )
                    {
                        break;
                    }
                }

                if ( k >= count )
                {
                    mGamePad[ j ].Reset();
                }
            }
        }

        // Check for added gamepads
        for( unsigned int j = 0; j < count; ++j )
        {
            ComPtr<IGamepad> pad;
            hr = pads->GetAt( j, pad.GetAddressOf() );
            if ( SUCCEEDED(hr) )
            {
                size_t empty = MAX_PLAYER_COUNT;
                size_t k = 0;
                for( ; k < MAX_PLAYER_COUNT; ++k )
                {
                    if ( mGamePad[ k ] == pad )
                    {
                        break;
                    }
                    else if ( !mGamePad[ k ] )
                    {
                        if ( empty >= MAX_PLAYER_COUNT )
                            empty = k;
                    }
                }

                if ( k >= MAX_PLAYER_COUNT )
                {
                    if ( empty >= MAX_PLAYER_COUNT )
                    {
                        throw std::exception( "Too many gamepads found" );
                    }

                    mGamePad[ empty ] = pad;
                }
            }
        }
    }

    Microsoft::WRL::ComPtr<ABI::Windows::Xbox::Input::IGamepadStatics> mStatics;
    Microsoft::WRL::ComPtr<ABI::Windows::Xbox::Input::IGamepad> mGamePad[ MAX_PLAYER_COUNT ];

    EventRegistrationToken mAddedToken;
    EventRegistrationToken mRemovedToken;

    static HANDLE s_changed;

#ifndef _TITLE
    static HRESULT GamepadAdded( IInspectable *, ABI::Windows::Xbox::Input::IGamepadAddedEventArgs * )
    {
        SetEvent( s_changed );
        return S_OK;
    }

    static HRESULT GamepadRemoved( IInspectable *, ABI::Windows::Xbox::Input::IGamepadRemovedEventArgs* )
    {
        SetEvent( s_changed );
        return S_OK;
    }
#endif
};

HANDLE GamePad::Impl::s_changed = nullptr;


#elif defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP)

//======================================================================================
// Null device for Windows Phone
//======================================================================================

class GamePad::Impl
{
public:
    void GetState(int player, _Out_ State& state, DeadZone)
    {
        UNREFERENCED_PARAMETER(player);

        memset( &state, 0, sizeof(State) );
    }

    void GetCapabilities(int player, _Out_ Capabilities& caps)
    {
        UNREFERENCED_PARAMETER(player);

        memset( &caps, 0, sizeof(Capabilities) );
    }

    bool SetVibration(int player, float leftMotor, float rightMotor)
    {
        UNREFERENCED_PARAMETER(player);
        UNREFERENCED_PARAMETER(leftMotor);
        UNREFERENCED_PARAMETER(rightMotor);

        return false;
    }

    void Suspend()
    {
    }

    void Resume()
    {
    }
};


#else

//======================================================================================
// XInput
//======================================================================================

#include <xinput.h>

static_assert( GamePad::MAX_PLAYER_COUNT == XUSER_MAX_COUNT, "xinput.h mismatch" );

class GamePad::Impl
{
public:
    Impl()
    {
        for( size_t j = 0; j < XUSER_MAX_COUNT; ++j )
        {
            mConnected[ j ] = false;
            mLastReadTime[ j ] = 0;
        }
    }

    void GetState( int player, _Out_ State& state, DeadZone deadZoneMode )
    {
        if ( !ThrottleRetry(player) )
        {
            XINPUT_STATE xstate;
            DWORD result = XInputGetState( DWORD(player), &xstate );
            if ( result == ERROR_DEVICE_NOT_CONNECTED )
            {
                mConnected[ player ] = false;
                mLastReadTime[ player ] = GetTickCount64();
            }
            else
            {
                mConnected[ player ] = true;

                state.connected = true;
                state.packet = xstate.dwPacketNumber;

                WORD xbuttons = xstate.Gamepad.wButtons;
                state.buttons.a = (xbuttons & XINPUT_GAMEPAD_A) != 0;
                state.buttons.b = (xbuttons & XINPUT_GAMEPAD_B) != 0;
                state.buttons.x = (xbuttons & XINPUT_GAMEPAD_X) != 0;
                state.buttons.y = (xbuttons & XINPUT_GAMEPAD_Y) != 0;
                state.buttons.leftStick = (xbuttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
                state.buttons.rightStick = (xbuttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
                state.buttons.leftShoulder = (xbuttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
                state.buttons.rightShoulder = (xbuttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
                state.buttons.back = (xbuttons & XINPUT_GAMEPAD_BACK) != 0;
                state.buttons.start = (xbuttons & XINPUT_GAMEPAD_START) != 0;

                state.dpad.up = (xbuttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
                state.dpad.down = (xbuttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
                state.dpad.right = (xbuttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
                state.dpad.left = (xbuttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;

                if ( deadZoneMode == DEAD_ZONE_NONE )
                {
                    state.triggers.left = ApplyLinearDeadZone( float(xstate.Gamepad.bLeftTrigger), 255.f, 0.f );
                    state.triggers.right = ApplyLinearDeadZone( float(xstate.Gamepad.bRightTrigger), 255.f, 0.f );
                }
                else
                {
                    state.triggers.left = ApplyLinearDeadZone( float(xstate.Gamepad.bLeftTrigger), 255.f, float(XINPUT_GAMEPAD_TRIGGER_THRESHOLD) );
                    state.triggers.right = ApplyLinearDeadZone( float(xstate.Gamepad.bRightTrigger), 255.f, float(XINPUT_GAMEPAD_TRIGGER_THRESHOLD) );
                }
            
                ApplyStickDeadZone( float(xstate.Gamepad.sThumbLX), float(xstate.Gamepad.sThumbLY),
                                    deadZoneMode, 32767.f, float(XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE),
                                    state.thumbSticks.leftX, state.thumbSticks.leftY );

                ApplyStickDeadZone( float(xstate.Gamepad.sThumbRX), float(xstate.Gamepad.sThumbRY),
                                    deadZoneMode, 32767.f, float(XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE),
                                    state.thumbSticks.rightX, state.thumbSticks.rightY );

                return;
            }
        }

        memset( &state, 0, sizeof(State) );
    }

    void GetCapabilities( int player, _Out_ Capabilities& caps )
    {
        if ( !ThrottleRetry(player) )
        {
            XINPUT_CAPABILITIES xcaps;
            DWORD result = XInputGetCapabilities( DWORD(player), 0, &xcaps );
            if ( result == ERROR_DEVICE_NOT_CONNECTED )
            {
                mConnected[ player ] = false;
                mLastReadTime[ player ] = GetTickCount64();
            }
            else
            {
                mConnected[ player ] = true;

                caps.connected = true;
                caps.id = uint64_t( player );
                if ( xcaps.Type == XINPUT_DEVTYPE_GAMEPAD )
                {
                    static_assert(Capabilities::GAMEPAD == XINPUT_DEVSUBTYPE_GAMEPAD, "xinput.h mismatch");
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
                    static_assert( XINPUT_DEVSUBTYPE_WHEEL == Capabilities::WHEEL, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_ARCADE_STICK == Capabilities::ARCADE_STICK, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_FLIGHT_STICK == Capabilities::FLIGHT_STICK, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_DANCE_PAD == Capabilities::DANCE_PAD, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_GUITAR == Capabilities::GUITAR, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE == Capabilities::GUITAR_ALTERNATE, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_DRUM_KIT == Capabilities::DRUM_KIT, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_GUITAR_BASS == Capabilities::GUITAR_BASS, "xinput.h mismatch");
                    static_assert( XINPUT_DEVSUBTYPE_ARCADE_PAD == Capabilities::ARCADE_PAD, "xinput.h mismatch");
#endif

                    caps.gamepadType = Capabilities::Type(xcaps.SubType);
                }

                return;
            }
        }

        memset( &caps, 0, sizeof(Capabilities) );
    }

    bool SetVibration( int player, float leftMotor, float rightMotor )
    {
        if ( ThrottleRetry(player) )
        {
            return false;
        }

        XINPUT_VIBRATION xvibration;
        xvibration.wLeftMotorSpeed = WORD( leftMotor * 0xFFFF );
        xvibration.wRightMotorSpeed = WORD( rightMotor * 0xFFFF );
        DWORD result = XInputSetState( DWORD(player), &xvibration );
        if ( result == ERROR_DEVICE_NOT_CONNECTED )
        {
            mConnected[ player ] = false;
            mLastReadTime[ player ] = GetTickCount64();
            return false;
        }
        else
        {
            mConnected[ player ] = true;
            return (result == ERROR_SUCCESS);
        }
    }

    void Suspend()
    {
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
        XInputEnable( FALSE );
#endif
    }

    void Resume()
    {
#if (_WIN32_WINNT >= _WIN32_WINNT_WIN8)
        XInputEnable( TRUE );
#endif
    }

private:
    bool        mConnected[ XUSER_MAX_COUNT ];
    ULONGLONG   mLastReadTime[ XUSER_MAX_COUNT ];

    bool ThrottleRetry( int player )
    {
        if ( ( player < 0 ) || ( player >= XUSER_MAX_COUNT ) )
            return true;

        if ( mConnected[ player ] )
            return false;

        ULONGLONG time = GetTickCount64();

        for( size_t j = 0; j < XUSER_MAX_COUNT; ++j )
        {
            if ( !mConnected[j] )
            {
                LONGLONG delta = time - mLastReadTime[j];

                LONGLONG interval = 1000;
                if ( (int)j != player )
                    interval /= 4;

                if ( (delta >= 0) && (delta < interval) )
                    return true;
            }
        }

        return false;
    }
};

#endif


// Public constructor.
GamePad::GamePad()
    : pImpl( new Impl() )
{
}


// Move constructor.
GamePad::GamePad(GamePad&& moveFrom)
  : pImpl(std::move(moveFrom.pImpl))
{
}


// Move assignment.
GamePad& GamePad::operator= (GamePad&& moveFrom)
{
    pImpl = std::move(moveFrom.pImpl);
    return *this;
}


// Public destructor.
GamePad::~GamePad()
{
}


GamePad::State GamePad::GetState(int player, DeadZone deadZoneMode)
{
    State state;
    pImpl->GetState(player, state, deadZoneMode);
    return state;
}


GamePad::Capabilities GamePad::GetCapabilities(int player)
{
    Capabilities caps;
    pImpl->GetCapabilities(player, caps);
    return caps;
}


bool GamePad::SetVibration( int player, float leftMotor, float rightMotor )
{
    return pImpl->SetVibration( player, leftMotor, rightMotor );
}


void GamePad::Suspend()
{
    pImpl->Suspend();
}


void GamePad::Resume()
{
    pImpl->Resume();
}


//======================================================================================
// ButtonStateTracker
//======================================================================================

#define UPDATE_BUTTON_STATE(field) field = static_cast<ButtonState>( ( !!state.buttons.field ) | ( ( !!state.buttons.field ^ !!lastState.buttons.field ) << 1 ) );

void GamePad::ButtonStateTracker::Update( const GamePad::State& state )
{
    UPDATE_BUTTON_STATE(a);

    assert( ( !state.buttons.a && !lastState.buttons.a ) == ( a == UP ) );
    assert( ( state.buttons.a && lastState.buttons.a ) == ( a == HELD ) );
    assert( ( !state.buttons.a && lastState.buttons.a ) == ( a == RELEASED ) );
    assert( ( state.buttons.a && !lastState.buttons.a ) == ( a == PRESSED ) );

    UPDATE_BUTTON_STATE(b);
    UPDATE_BUTTON_STATE(x);
    UPDATE_BUTTON_STATE(y);

    UPDATE_BUTTON_STATE(leftStick);
    UPDATE_BUTTON_STATE(rightStick);

    UPDATE_BUTTON_STATE(leftShoulder);
    UPDATE_BUTTON_STATE(rightShoulder);

    UPDATE_BUTTON_STATE(back);
    UPDATE_BUTTON_STATE(start);

    dpadUp = static_cast<ButtonState>( ( !!state.dpad.up ) | ( ( !!state.dpad.up ^ !!lastState.dpad.up ) << 1 ) );
    dpadDown = static_cast<ButtonState>( ( !!state.dpad.down ) | ( ( !!state.dpad.down ^ !!lastState.dpad.down ) << 1 ) );
    dpadLeft = static_cast<ButtonState>( ( !!state.dpad.left ) | ( ( !!state.dpad.left ^ !!lastState.dpad.left ) << 1 ) );
    dpadRight = static_cast<ButtonState>( ( !!state.dpad.right ) | ( ( !!state.dpad.right ^ !!lastState.dpad.right ) << 1 ) );

    assert( ( !state.dpad.up && !lastState.dpad.up ) == ( dpadUp == UP ) );
    assert( ( state.dpad.up && lastState.dpad.up ) == ( dpadUp == HELD ) );
    assert( ( !state.dpad.up && lastState.dpad.up ) == ( dpadUp == RELEASED ) );
    assert( ( state.dpad.up && !lastState.dpad.up ) == ( dpadUp == PRESSED ) );

    lastState = state;
}

#undef UPDATE_BUTTON_STATE


void GamePad::ButtonStateTracker::Reset()
{
    memset( this, 0, sizeof(ButtonStateTracker) );
}
