/*
 * Copyright (c) 2009 Matthew Iselin
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

// netkeys targets a minimum platform of Windows 2000
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0500 // Minimum platform is Win 2000
#endif

#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#include <time.h>
#include <string>
#include <iostream>
#include <string>

using namespace std;

/**
 * MESSAGE_MAGIC serves two purposes: one, ensuring the correct structures
 * are in use, and two, ensuring that these structures are not malformed.
 */
#define MESSAGE_MAGIC   0xdeadbeef

/**
 * These two definitions are merely codes for each message between the two
 * hosts.
 */
#define MESSAGE_KEYUP   0x00000001
#define MESSAGE_KEYDOWN 0x00000002

/// \todo Specify port on the command line
#define SYSTEM_PORT     23000

/**
 * Packet structure for the key press/release message transfer.
 */
struct keyMessage
{
    DWORD magic;
    DWORD code;
};

/**
 * Low-level keyboard hook. Global so it can be deleted via an atexit setup
 * rather than requiring a full message pump with special WM_CLOSE handling.
 */
HHOOK myHook = NULL;

/**
 * Global network socket.
 *
 * Probably not the greatest way in the world to do things. I'd like to C++
 * this up a bit.
 */
int mySocket = -1;

/**
 * Remote IP for all networking.
 */
string remoteIp = "";

/**
 * Horrible, horrible, horrible method of figuring out whether we need to
 * send a message to the client. std::map for *all* VK_ codes would be *far*
 * more efficient, and it looks nicer too.
 */
volatile bool isDown = false;

/** KeyboardProc: Low-level keyboard hook handler */
LRESULT __declspec(dllexport)__stdcall  CALLBACK KeyboardProc(int nCode,
                                                              WPARAM wParam,
                                                              LPARAM lParam);

/** death: atexit routine */
void death();

/** initWinsock: Initialises Windows sockets */
/// \todo Move into a class!
int initWinsock();

/** getSocket: Set & return mySocket if not set, return it if so. */
int getSocket();

/** startListen: Listen on the given port (don't use) */
int startListen(int port);

/**
 * connectRemote
 *
 * "Connect" to the given host on the given port. Because this is all done over
 * UDP this is more or less defining the IP with which we'll be performing send
 * and recv operations.
 */
int connectRemote(const char *host, int port);

/** sendMessage: Sends the given message. */
int sendMessage(const char *msg, int len);

/** recvMessage: Blocks and receives a message */
int recvMessage(char *buff, int msglen);

/** returnSocket: Cleans up mySocket */
int returnSocket();

/** destroyWinsock: Cleans up Windows Sockets */
int destroyWinsock();

/**
 * hookAndSend
 *
 * This function is the "server" side of things. It creates a low-level
 * keyboard hook that is used to pick up every key the user presses.
 * Basically, it dumps a little bit of information to the console and then
 * takes over the Windows message pump for the console.
 */
void hookAndSend()
{
    connectRemote(remoteIp.c_str(), SYSTEM_PORT);

    // Grab our window handle
    HWND hWnd = GetConsoleWindow();
    HINSTANCE hInst = (HINSTANCE) GetWindowLong(hWnd, GWL_HINSTANCE);

    // Spruce up the window
    SetConsoleTitle("netkeys");

    // Install the hook
    myHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInst, 0);
    if(!myHook)
    {
        cerr << "Obtaining the keyboard hook failed." << endl;
        cerr << "Press any key to continue." << endl;
        _getch();
        return;
    }

    // Tell the user what's happening
    cout << "netkeys is now running." << endl;
    cout << "Just close this window when you're done." << endl;
    cout << "I'll automatically shut everything down." << endl;

    // Main message pump
    MSG msg;
    while(GetMessage(&msg, hWnd, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Shouldn't really get here
	return;
}

/**
 * listenForKeys
 *
 * This function is the "client" side of things. It merely listens for any
 * incoming key messages, ensures that they're valid, and then injects them
 * into the host key press queue.
 */
/// \todo Allow the user to type something like "quit" or whatever to exit,
///       perhaps via a small command-line monitor to also see some statistics
/// \todo It's real easy to kill things right now: just exit the client while
///       holding down a key on the host to find out why (the key remains held
///       down, as the keyup event is never received).
void listenForKeys()
{
    startListen(SYSTEM_PORT);

    struct keyMessage s;
    while(recvMessage((char*) &s, sizeof(s)) != -1)
    {
        if(s.magic != MESSAGE_MAGIC)
        {
            /// \todo Better error-handling, more consistent, etc...
            cerr << "netkeys: non-fatal: Got a bad packet." << endl;
            cerr << "packet magic: " << hex << s.magic << dec << "]" << endl;
            continue;
        }

        INPUT wrapper;
        wrapper.type = INPUT_KEYBOARD;

        KEYBDINPUT myInput;
        myInput.dwExtraInfo = 0;
        myInput.dwFlags = KEYEVENTF_EXTENDEDKEY;
        myInput.time = 0;
        myInput.wVk = VK_RCONTROL;
        myInput.wScan = 0x1D;

        if(s.code == MESSAGE_KEYUP)
            myInput.dwFlags |= KEYEVENTF_KEYUP;

        wrapper.ki = myInput;
        SendInput(1, &wrapper, sizeof(wrapper));
    }
}

int main(int argc, char* argv[])
{
    // Initialise Winsock and grab a socket
    if(initWinsock())
    {
        cerr << "Couldn't initialise the socket layer." << endl;
        cerr << "Press any key to exit." << endl;
        _getch();
        return 1;
    }
    getSocket();

    // Install cleanup to run when we die
    atexit(death);

    // Check for and handle input parameters
    /// \todo C++ify
    bool bClient = false;
    for(int i = 1; i < argc; i++)
    {
        if(_stricmp(argv[i], "--client") == 0)
        {
            bClient = true;
        }
        else if(_stricmp(argv[i], "--ip") == 0)
        {
            if((i + 1) < argc)
            {
                remoteIp = argv[i + 1];
                i++;
            }
            else
            {
                cout << "The 'ip' argument requires an IP." << endl;
                return 1;
            }
        }
    }

    if(remoteIp == "")
    {
        cout << "No IP specified, quitting." << endl;
        return 1;
    }

    // Client?
    if(bClient)
    {
        listenForKeys();
        returnSocket();
        destroyWinsock();
    }
    // No, we're not the client. Hook and serve.
    else
        hookAndSend();
    return 0;
}

LRESULT __declspec(dllexport)__stdcall  CALLBACK KeyboardProc(int nCode,
                                                              WPARAM wParam,
                                                              LPARAM lParam)
{
    if(nCode == HC_ACTION)
    {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT*) lParam;
        if(p->vkCode == VK_RCONTROL)
        {
            if(wParam == WM_KEYDOWN)
            {
                if(!isDown)
                {
                    isDown = true;
                    struct keyMessage s;
                    s.code = MESSAGE_KEYDOWN;
                    s.magic = MESSAGE_MAGIC;
                    sendMessage((const char*) &s, sizeof(s));
                }
            }
            if(wParam == WM_KEYUP)
            {
                struct keyMessage s;
                s.code = MESSAGE_KEYUP;
                s.magic = MESSAGE_MAGIC;
                sendMessage((const char*) &s, sizeof(s));

                isDown = false;
            }
        }
    }
    return CallNextHookEx(myHook, nCode, wParam, lParam);
}

void death()
{
    if(myHook)
        UnhookWindowsHookEx(myHook);

    returnSocket();
    destroyWinsock();
}

int initWinsock()
{
    WORD wVersionRequested;
    WSADATA wsaData;
    int err;

    /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
    wVersionRequested = MAKEWORD(2, 2);

    err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        /* Tell the user that we could not find a usable */
        /* Winsock DLL.                                  */
        cerr << "WSAStartup failed with error: " << err << endl;
        return 1;
    }

    /* Confirm that the WinSock DLL supports 2.2.*/
    /* Note that if the DLL supports versions greater    */
    /* than 2.2 in addition to 2.2, it will still return */
    /* 2.2 in wVersion since that is the version we      */
    /* requested.                                        */

    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
        /* Tell the user that we could not find a usable */
        /* WinSock DLL.                                  */
        cerr << "Could not find a usable version of Winsock.dll" << endl;
        WSACleanup();
        return 1;
    }
    else
        return 0;
}

int getSocket()
{
    if(mySocket == -1)
        mySocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    return mySocket;
}

int startListen(int port)
{
    if(mySocket < 0)
        return -1;

    sockaddr_in service;
    service.sin_family = AF_INET;
    service.sin_addr.s_addr = INADDR_ANY;
    service.sin_port = htons(port);
    if(bind(mySocket, (SOCKADDR*) &service, sizeof(service)) == SOCKET_ERROR)
        return -1;

    return 0;
}

int connectRemote(const char *host, int port)
{
    if(mySocket < 0)
        return -1;

    sockaddr_in service;
    service.sin_family = AF_INET;

    struct hostent *ent;
    if((service.sin_addr.s_addr = inet_addr(host)) == INADDR_NONE)
    {
        if((ent = gethostbyname(host)) != NULL)
        {
            service.sin_addr.s_addr = inet_addr(ent->h_addr_list[0]);
        }
        else
            return -1;
    }

    service.sin_port = htons(port);

    return connect(mySocket, (SOCKADDR*) &service, sizeof(service));
}

int sendMessage(const char *msg, int len)
{
   if(mySocket < 0)
       return -1;

   return send(mySocket, msg, len, 0);
}

int recvMessage(char *buff, int msglen)
{
   if(mySocket < 0)
       return -1;

   return recv(mySocket, buff, msglen, 0);
}

int returnSocket()
{
    if(mySocket >= 0)
    {
        shutdown(mySocket, SD_BOTH);
        closesocket(mySocket);
    }
    return 0;
}

int destroyWinsock()
{
    return WSACleanup();
}
