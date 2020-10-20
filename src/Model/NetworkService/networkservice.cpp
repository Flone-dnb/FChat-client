﻿// This file is part of the Silent.
// Copyright Aleksandr "Flone" Tretyakov (github.com/Flone-dnb).
// Licensed under the ZLib license.
// Refer to the LICENSE file included.

#include "networkservice.h"

// STL
#include <thread>


// ============== Network ==============
// Sockets and stuff
#include <winsock2.h>

// Adress translation
#include <ws2tcpip.h>

// Winsock 2 Library
#pragma comment(lib,"Ws2_32.lib")

// Custom
#include "View/MainWindow/mainwindow.h"
#include "Model/AudioService/audioservice.h"
#include "Model/SettingsManager/settingsmanager.h"
#include "Model/SettingsManager/SettingsFile.h"
#include "Model/net_params.h"
#include "Model/OutputTextType.h"
#include "View/CustomList/SListItemUser/slistitemuser.h"
#include "View/CustomList/SListItemRoom/slistitemroom.h"
#include "Model/User.h"

// External
#include "AES/AES.h"
#include "integer/integer.h"


enum CONNECT_MESSAGE  {
    CM_USERNAME_INUSE       = 0,
    CM_SERVER_FULL          = 2,
    CM_WRONG_CLIENT         = 3,
    CM_SERVER_INFO          = 4,
    CM_NEED_PASSWORD        = 5
};

enum ROOM_COMMAND
{
    RC_ENTER_ROOM           = 15,
    RC_ENTER_ROOM_WITH_PASS = 16,

    RC_CAN_ENTER_ROOM       = 20,
    RC_ROOM_IS_FULL         = 21,
    RC_PASSWORD_REQ         = 22,
    RC_WRONG_PASSWORD       = 23,

    RC_USER_ENTERS_ROOM     = 25,
    RC_SERVER_MOVED_ROOM    = 26,
    RC_SERVER_DELETES_ROOM  = 27,
    RC_SERVER_CREATES_ROOM  = 28,
    RC_SERVER_CHANGES_ROOM  = 29
};

enum SERVER_MESSAGE  {
    SM_NEW_USER             = 0,
    SM_SOMEONE_DISCONNECTED = 1,
    SM_CAN_START_UDP        = 2,
    SM_SPAM_NOTICE          = 3,
    SM_PING                 = 8,
    SM_KEEPALIVE            = 9,
    SM_USERMESSAGE          = 10,
    SM_KICKED               = 11,
    SM_WRONG_PASSWORD_WAIT  = 12,
    SM_GLOBAL_MESSAGE       = 13
};

enum USER_DISCONNECT_REASON
{
    UDR_DISCONNECT          = 0,
    UDR_LOST                = 1,
    UDR_KICKED              = 2
};

enum UDP_SERVER_MESSAGE
{
    UDP_SM_PREPARE          = -1,
    UDP_SM_PING             = 0,
    UDP_SM_FIRST_PING       = -2,
    UDP_SM_USER_READY       = -3
};


// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------


NetworkService::NetworkService(MainWindow* pMainWindow, AudioService* pAudioService, SettingsManager* pSettingsManager)
{
    this ->pMainWindow      = pMainWindow;
    this ->pAudioService    = pAudioService;
    this ->pSettingsManager = pSettingsManager;
    pThisUser               = nullptr;

    pAES = new AES(128);
    pRndGen = new std::mt19937_64( std::random_device{}() );


    clientVersion = CLIENT_VERSION;


    bWinSockLaunched = false;
    bTextListen      = false;
    bVoiceListen     = false;
}

NetworkService::~NetworkService()
{
    delete pAES;
    delete pRndGen;
}




std::string NetworkService::getClientVersion() const
{
    return clientVersion;
}

std::string NetworkService::getUserName() const
{
    if (pThisUser)
    {
        return pThisUser ->sUserName;
    }
    else
    {
        return "";
    }
}

SListItemRoom *NetworkService::getUserRoom() const
{
    if (pThisUser)
    {
        return dynamic_cast<SListItemUser*>(pThisUser->pListWidgetItem)->getRoom();
    }
    else
    {
        return nullptr;
    }
}

size_t NetworkService::getOtherUsersVectorSize() const
{
    return vOtherUsers .size();
}

User *NetworkService::getOtherUser(size_t i) const
{
    return vOtherUsers[i];
}

std::mutex *NetworkService::getOtherUsersMutex()
{
    return &mtxOtherUsers;
}

void NetworkService::eraseDisconnectedUser(std::string sUserName, char cDisconnectType)
{
    // Find this user in the vOtherUsers vector.

    User*  pDisconnectedUser = nullptr;
    size_t iUserPosInVector  = 0;

    mtxOtherUsers .lock();


    for (size_t i = 0;   i < vOtherUsers .size();   i++)
    {
        if ( vOtherUsers[i] ->sUserName == sUserName )
        {
            pDisconnectedUser = vOtherUsers[i];
            iUserPosInVector  = i;

            break;
        }
    }


    // Delete user from screen & AudioService & play audio sound.

    if (pDisconnectedUser)
    {
        SListItemUser* pItem = pDisconnectedUser ->pListWidgetItem;

        if (pItem->getRoom() == pThisUser->pListWidgetItem->getRoom())
        {
            pAudioService ->playConnectDisconnectSound(false);
        }

        pAudioService ->deleteUserAudio     (pDisconnectedUser);

        delete pDisconnectedUser;
        vOtherUsers .erase( vOtherUsers .begin() + iUserPosInVector );

        pMainWindow   ->deleteUserFromList  (pItem);

        if (pSettingsManager ->getCurrentSettings() ->bShowConnectDisconnectMessage)
        {
            pMainWindow   ->showUserDisconnectNotice(sUserName, SilentMessageColor(true), cDisconnectType);
        }
    }

    mtxOtherUsers. unlock();
}

void NetworkService::clearWinsockAndThisUser()
{
    WSACleanup();

    delete pThisUser;
    pThisUser        = nullptr;

    bWinSockLaunched = false;
}

void NetworkService::start(std::string adress, std::string port, std::string userName, std::wstring sPass)
{
    if (bTextListen)
    {
        disconnect();
    }


    // Disable UI
    pMainWindow ->enableInteractiveElements(false, false);




    // Start WinSock2 (ver. 2.2)

    WSADATA WSAData;
    int returnCode = 0;

    returnCode = WSAStartup(MAKEWORD(2, 2), &WSAData);

    if (returnCode != 0)
    {
        pMainWindow ->printOutput(std::string( "NetworkService::start()::WSAStartup() function failed and returned: "
                                               + std::to_string(WSAGetLastError())
                                               + ".\nTry again.\n"), SilentMessageColor(false) );
    }
    else
    {
        bWinSockLaunched = true;




        pThisUser = new User("", 0, nullptr);

        // Create the IPv4 TCP socket

        pThisUser ->sockUserTCP = socket(AF_INET, SOCK_STREAM, 0);

        if (pThisUser ->sockUserTCP == INVALID_SOCKET)
        {
            pMainWindow->printOutput("NetworkService::start()::socket() function failed and returned: "
                                     + std::to_string(WSAGetLastError())
                                     + ".\nTry again.\n", SilentMessageColor(false));

            clearWinsockAndThisUser();

            pMainWindow->enableInteractiveElements(true, false);
        }
        else
        {
            std::string sFormattedAdress = formatAdressString(adress);

            std::thread connectThread(&NetworkService::connectTo, this, sFormattedAdress, port, userName, sPass);
            connectThread .detach();
        }
    }
}

void NetworkService::connectTo(std::string adress, std::string port, std::string userName,  std::wstring sPass)
{
    int returnCode = 0;




    // Fill sockaddr_in

    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *result = nullptr;


    INT dResult = getaddrinfo(adress .c_str(), port .c_str(), &hints, &result);
    if ( dResult != 0 )
    {
        pMainWindow ->printOutput("NetworkService::connectTo::getaddrinfo() failed. Error code: "
                                  + std::to_string(WSAGetLastError()) + ".\n", SilentMessageColor(false), true);

        clearWinsockAndThisUser();

        pMainWindow->enableInteractiveElements(true, false);

        return;
    }



    // Copy result to This User

    memset( pThisUser ->addrServer .sin_zero, 0, sizeof(pThisUser ->addrServer.sin_zero) );
    std::memcpy(&pThisUser ->addrServer, result ->ai_addr, result ->ai_addrlen);




    pMainWindow->printOutput(std::string("Connecting...\n"
                                         "Please wait, the server might be busy if a lot of people is entering the server right now.\n"), SilentMessageColor(false), true);



    // Connect

    returnCode = connect(pThisUser ->sockUserTCP, result ->ai_addr, result ->ai_addrlen);

    freeaddrinfo(result);

    if (returnCode == SOCKET_ERROR)
    {
        returnCode = WSAGetLastError();

        if (returnCode == 10060)
        {
            pMainWindow->printOutput("Time out.\nTry again.\n",
                                     SilentMessageColor(false), true);
        }
        else if (returnCode == 10051)
        {
            pMainWindow->printOutput("NetworkService::connectTo()::connect() function failed and returned: "
                                     + std::to_string(returnCode)
                                     + ".\nPossible cause: no internet.\nTry again.\n",
                                     SilentMessageColor(false), true);
        }
        else
        {
            pMainWindow->printOutput("NetworkService::connectTo()::connect() function failed and returned: "
                                     + std::to_string(returnCode)
                                     + ".\nTry again.\n",
                                     SilentMessageColor(false), true);
        }

        pMainWindow->enableInteractiveElements(true, false);

        clearWinsockAndThisUser();
    }
    else
    {
        // Disable Nagle algorithm for connected socket

        BOOL bOptVal = true;
        int bOptLen  = sizeof(BOOL);

        returnCode = setsockopt(pThisUser ->sockUserTCP, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast <char*> (&bOptVal), bOptLen);

        if (returnCode == SOCKET_ERROR)
        {
            pMainWindow ->printOutput("NetworkService::connectTo()::setsockopt (Nagle algorithm) failed and returned: "
                                     + std::to_string(WSAGetLastError())
                                     + ".\nTry again.\n",
                                     SilentMessageColor(false),
                                     true);

            closesocket(pThisUser ->sockUserTCP);
            clearWinsockAndThisUser();
            pMainWindow ->enableInteractiveElements(true, false);
        }
        else
        {
            // Send version & user name

            char versionAndNameBuffer[MAX_NAME_LENGTH + MAX_VERSION_STRING_LENGTH + (UCHAR_MAX * 2) + 2];
            memset(versionAndNameBuffer, 0, MAX_NAME_LENGTH + MAX_VERSION_STRING_LENGTH + (UCHAR_MAX * 2) + 2);

            int iPos = 0;

			// Version
            char versionStringSize  = static_cast <char> (clientVersion .size());
            versionAndNameBuffer[iPos] = versionStringSize;
            iPos += sizeof(versionStringSize);

            std::memcpy(versionAndNameBuffer + iPos, clientVersion .c_str(), static_cast <unsigned long long> (versionStringSize));
            iPos += clientVersion .size();
			


			// Client name
			char nameStringSize = static_cast <char> (userName .size());
            versionAndNameBuffer[iPos] = nameStringSize;
            iPos += sizeof(nameStringSize);

            std::memcpy(versionAndNameBuffer + iPos, userName .c_str(),      userName .size());
            iPos += userName .size();
			


			// Password (optional)
            char passwordStringSize = static_cast <char> (sPass .size());
            versionAndNameBuffer[iPos] = passwordStringSize;
            iPos += sizeof(passwordStringSize);

            std::memcpy(versionAndNameBuffer + iPos, sPass .c_str(), passwordStringSize * 2);
            iPos += sPass .size() * 2;

            send(pThisUser ->sockUserTCP, versionAndNameBuffer, iPos, 0);




            // Receive answer

            const int iMaxBufferSize = MAX_TCP_BUFFER_SIZE;
            char readBuffer[iMaxBufferSize];
            memset(readBuffer, 0, iMaxBufferSize);

            int iReceivedSize = recv(pThisUser ->sockUserTCP, readBuffer, 1, 0);

            if (readBuffer[0] == CM_USERNAME_INUSE)
            {
                // This user name is already in use. Disconnect.

                if ( recv(pThisUser ->sockUserTCP, readBuffer, 1, 0) == 0 )
                {
                    shutdown(pThisUser ->sockUserTCP, SD_SEND);
                }

                pMainWindow ->printOutput("\nA user with this name is already present on the server. Select another name.",
                                         SilentMessageColor(false),
                                         true);

                pMainWindow ->enableInteractiveElements(true, false);
                closesocket(pThisUser ->sockUserTCP);
                clearWinsockAndThisUser();
            }
            else if (readBuffer[0] == CM_SERVER_FULL)
            {
                // Server is full

                if ( recv(pThisUser ->sockUserTCP, readBuffer, 1, 0) == 0 )
                {
                    shutdown(pThisUser ->sockUserTCP, SD_SEND);
                }

                pMainWindow ->printOutput("\nServer is full.",
                                          SilentMessageColor(false),
                                          true);

                pMainWindow ->enableInteractiveElements(true, false);
                closesocket(pThisUser ->sockUserTCP);
                clearWinsockAndThisUser();
            }
            else if (readBuffer[0] == CM_WRONG_CLIENT)
            {
                // Wrong client version

                char serverVersionSize = 0;
                recv(pThisUser ->sockUserTCP, &serverVersionSize, 1, 0);

                char versionBuffer[MAX_VERSION_STRING_LENGTH + 1];
                memset(versionBuffer, 0, MAX_VERSION_STRING_LENGTH + 1);

                recv(pThisUser ->sockUserTCP, versionBuffer, MAX_VERSION_STRING_LENGTH, 0);

                if (recv(pThisUser ->sockUserTCP, readBuffer, 1, 0) == 0)
                {
                    shutdown(pThisUser ->sockUserTCP,SD_SEND);
                }

                pMainWindow ->printOutput("\nYour Silent version (" + clientVersion + ") does not match the server's "
                                         "supported client version (" + std::string(versionBuffer) + ").\n"
                                         "Please change your Silent version to " + std::string(versionBuffer)
                                         + " if you want to connect to this server.",
                                         SilentMessageColor(false), true);

                pMainWindow ->enableInteractiveElements(true, false);
                closesocket(pThisUser ->sockUserTCP);
                clearWinsockAndThisUser();
            }
            else if (readBuffer[0] == CM_NEED_PASSWORD)
            {
                // Server has password and our password is wrong

                if ( recv(pThisUser ->sockUserTCP, readBuffer, 1, 0) == 0 )
                {
                    shutdown(pThisUser ->sockUserTCP, SD_SEND);
                }

                pMainWindow ->printOutput("\nThe Server has a password.\n"
                                          "You either not entered a password or it was wrong.",
                                          SilentMessageColor(false),
                                          true);

                pMainWindow ->enableInteractiveElements(true, false);
                closesocket(pThisUser ->sockUserTCP);
                clearWinsockAndThisUser();
            }
            else if (readBuffer[0] == CM_SERVER_INFO)
            {
                // Receive packet size

                iReceivedSize = recv(pThisUser ->sockUserTCP, readBuffer, 2, 0);

                unsigned short int iPacketSize = 0;
                std::memcpy(&iPacketSize, readBuffer, 2);

                memset(readBuffer, 0, iMaxBufferSize);



                // Receive data

                iReceivedSize = recv(pThisUser ->sockUserTCP, readBuffer, iPacketSize, 0);



                pMainWindow   ->printOutput("Connected.\n"
                                            "You are queued to enter the server, first, the server will process everyone who entered before you.\n"
                                            "Waiting to establish a secure connection, please wait...\n",
                                         SilentMessageColor(false), true);

                // Don't process this data now.

                // Generate secret key.

                std::uniform_int_distribution<> uid(100, 500); // also change in server

                int b = uid(*pRndGen);

                // Receive p, g values.

                char vKeyPGBuffer[sizeof(integer) * 2];
                memset(vKeyPGBuffer, 0, sizeof(integer) * 2);

                recv(pThisUser ->sockUserTCP, vKeyPGBuffer, sizeof(int) * 2, 0);

                int p, g;

                std::memcpy(&p, vKeyPGBuffer, sizeof(p));
                std::memcpy(&g, vKeyPGBuffer + sizeof(p), sizeof(g));

                integer B = pow(integer(g), b) % p;


                size_t iMaxKeyLength = 1000;
                short iStringSize = 0;
                char* pOpenKeyString = new char[sizeof(iStringSize) + iMaxKeyLength + 1];
                memset(pOpenKeyString, 0, sizeof(iStringSize) + iMaxKeyLength + 1);

                // Receive A.

                int iResult = recv(pThisUser ->sockUserTCP, reinterpret_cast<char*>(&iStringSize), sizeof(iStringSize), 0);
                if (iResult <= 0)
                {
                    // Something went wrong.

                    if ( recv(pThisUser ->sockUserTCP, readBuffer, 1, 0) == 0 )
                    {
                        shutdown(pThisUser ->sockUserTCP, SD_SEND);
                    }

                    pMainWindow ->printOutput("\nSomething went wrong on the server side.\n"
                                              "Try connecting again.",
                                              SilentMessageColor(false),
                                              true);

                    pMainWindow ->enableInteractiveElements(true, false);
                    closesocket(pThisUser ->sockUserTCP);
                    clearWinsockAndThisUser();

                    delete[] pOpenKeyString;

                    return;
                }

                recv(pThisUser ->sockUserTCP, pOpenKeyString, iStringSize, 0);


                integer A(pOpenKeyString, 10);

                // Send B.

                if (B.str().size() > iMaxKeyLength) // should not happen
                {
                    pMainWindow ->printOutput("Failed to establish a secure connection (client error).\nTry again.\n",
                                             SilentMessageColor(false),
                                             true);

                    closesocket(pThisUser ->sockUserTCP);
                    clearWinsockAndThisUser();
                    pMainWindow ->enableInteractiveElements(true, false);

                    delete[] pOpenKeyString;

                    return;
                }

                iStringSize = static_cast<short>(B.str().size());

                memset(pOpenKeyString, 0, sizeof(iStringSize) + iMaxKeyLength + 1);

                std::memcpy(pOpenKeyString, &iStringSize, sizeof(iStringSize));
                std::memcpy(pOpenKeyString + sizeof(iStringSize), B.str().c_str(), B.str().size());

                send(pThisUser ->sockUserTCP, pOpenKeyString, sizeof(iStringSize) + B.str().size(), 0);

                integer secret = pow(integer(A), b) % p;

                if (secret.str().size() >= 16)
                {
                    std::memcpy(vSecretAESKey, secret.str().c_str(), 16);
                }
                else
                {
                    std::string sSecret = secret.str();

                    size_t iFilledCount = 0;
                    size_t iCurrentIndex = 0;

                    while (iFilledCount != 16)
                    {
                        vSecretAESKey[iFilledCount] = sSecret[iCurrentIndex];

                        iFilledCount++;

                        if (iCurrentIndex == sSecret.size() - 1)
                        {
                            iCurrentIndex = 0;
                        }
                        else
                        {
                            iCurrentIndex++;
                        }
                    }
                }

                delete[] pOpenKeyString;



                // Sync with the server.

                // Translate socket to blocking mode

                u_long arg = false;
                ioctlsocket(pThisUser ->sockUserTCP, static_cast <long> (FIONBIO), &arg);

                // send "finished connecting" message.
                char message = 99;
                if (send(pThisUser ->sockUserTCP, &message, sizeof(message), 0) <= 0)
                {
                    pMainWindow ->printOutput("NetworkService::connectTo()::ioctsocket() (non-blocking mode) failed and returned: "
                                              + std::to_string(WSAGetLastError()) + ".\n",
                                              SilentMessageColor(false), true);

                    closesocket(pThisUser ->sockUserTCP);
                    clearWinsockAndThisUser();
                    pMainWindow ->enableInteractiveElements(true, false);

                    return;
                }

                // receive "finished connecting" message.
                if (recv(pThisUser ->sockUserTCP, &message, sizeof(message), 0) == 0)
                {
                    pMainWindow ->printOutput("NetworkService::connectTo()::recv(): "
                                              "the server waits too long for our response and therefore closes the connection.\n",
                                              SilentMessageColor(false), true);

                    closesocket(pThisUser ->sockUserTCP);
                    clearWinsockAndThisUser();
                    pMainWindow ->enableInteractiveElements(true, false);

                    return;
                }



                pMainWindow   ->printOutput("A secure connection has been established, the data transmitted over the network is encrypted.\n"
                                            "Received " + std::to_string(iReceivedSize + 3) + " bytes of data from the server.\n"
                                            "Waiting to connect to the text chat...\n",
                                         SilentMessageColor(false), true);



                // Translate socket to non-blocking mode

                arg = true;

                if ( ioctlsocket(pThisUser ->sockUserTCP, static_cast <long> (FIONBIO), &arg) == SOCKET_ERROR )
                {
                    pMainWindow ->printOutput("NetworkService::connectTo()::ioctsocket() (non-blocking mode) failed and returned: "
                                              + std::to_string(WSAGetLastError()) + ".\n",
                                              SilentMessageColor(false), true);

                    closesocket(pThisUser ->sockUserTCP);
                    clearWinsockAndThisUser();
                    pMainWindow ->enableInteractiveElements(true, false);

                    return;
                }


                // Prepare AudioService

                pAudioService ->prepareForStart();




                // READ online info

                int iReadBytes = 0;
                int iOnline    = 1; // '1' - this user.


                char roomCount = 0;
                std::memcpy(&roomCount, readBuffer + iReadBytes, 1);
                iReadBytes++;


                for (char i = 0;   i < roomCount;   i++)
                {
                    std::string  sRoomName   = "";
                    std::wstring sRoomPass   = L"";
                    unsigned short iMaxUsers = 0;


                    char roomNameSize = 0;
                    std::memcpy(&roomNameSize, readBuffer + iReadBytes, 1);
                    iReadBytes++;

                    char bufferForNames[MAX_NAME_LENGTH + 1];
                    memset(bufferForNames, 0, MAX_NAME_LENGTH + 1);

                    std::memcpy(bufferForNames, readBuffer + iReadBytes, roomNameSize);
                    iReadBytes += roomNameSize;

                    sRoomName = bufferForNames;



                    std::memcpy(&iMaxUsers, readBuffer + iReadBytes, 2);
                    iReadBytes += 2;


                    bool bFirstRoom = false;

                    if (i == 0)
                    {
                        bFirstRoom = true;
                    }

                    pMainWindow->addRoom(sRoomName, sRoomPass, iMaxUsers, bFirstRoom);




                    unsigned short iUsersInRoom = 0;
                    std::memcpy(&iUsersInRoom, readBuffer + iReadBytes, 2);
                    iReadBytes += 2;

                    iOnline += iUsersInRoom;

                    for (unsigned short j = 0; j < iUsersInRoom; j++)
                    {
                        unsigned char currentItemSize = 0;
                        std::memcpy(&currentItemSize, readBuffer + iReadBytes, 1);
                        iReadBytes++;

                        char rowText[MAX_NAME_LENGTH + 1];
                        memset(rowText, 0, MAX_NAME_LENGTH + 1);

                        std::memcpy(rowText, readBuffer + iReadBytes, currentItemSize);
                        iReadBytes += currentItemSize;



                        // Add new user.

                        std::string sNewUserName = std::string(rowText);

                        User* pNewUser = new User( sNewUserName, 0, pMainWindow ->addUserToRoomIndex(sNewUserName, i) );

                        vOtherUsers .push_back( pNewUser );

                        pAudioService ->setupUserAudio( pNewUser );
                    }
                }


                pMainWindow ->setOnlineUsersCount(iOnline);

                // Save this user

                pThisUser ->sUserName = userName;
                pThisUser ->pListWidgetItem = pMainWindow ->addUserToRoomIndex(userName, 0);

                pAudioService ->setupUserAudio( pThisUser );




                // Start listen thread

                pMainWindow ->printOutput("Connected to the text chat.\n"
                                          "Waiting to connect to the voice chat. Please wait...\n",
                                         SilentMessageColor(false),
                                         true);

                pMainWindow ->enableInteractiveElements(true, true);
                pMainWindow ->setConnectDisconnectButton(false);

                bTextListen = true;

                std::thread listenTextThread (&NetworkService::listenTCPFromServer, this);
                listenTextThread.detach();




                // We can start voice connection

                setupVoiceConnection();




                // Save user name to settings

                SettingsFile* pUpdatedSettings    = pSettingsManager ->getCurrentSettings();
                if (pUpdatedSettings)
                {
                    pUpdatedSettings ->sUsername      = userName;
                    pUpdatedSettings ->sConnectString = adress;
                    pUpdatedSettings ->iPort          = static_cast<unsigned short>(stoi(port));
                    pUpdatedSettings ->sPassword      = sPass;

                    pSettingsManager ->saveCurrentSettings();
                }




                // Start Keep-Alive clock

                lastTimeServerKeepAliveCame = clock();
                std::thread monitor(&NetworkService::serverMonitor, this);
                monitor .detach();
            }
        }
    }
}

void NetworkService::setupVoiceConnection()
{
    // Create UDP socket

    pThisUser ->sockUserUDP = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (pThisUser ->sockUserUDP == INVALID_SOCKET)
    {
        pMainWindow ->printOutput( "Cannot start voice connection.\n"
                                  "NetworkService::setupVoiceConnection::socket() error: "
                                  + std::to_string(WSAGetLastError()),
                                  SilentMessageColor(false),
                                  true );
        return;
    }
    else
    {
        // From documentation:
        // For a connectionless socket (for example, type SOCK_DGRAM), the operation performed by connect is merely to
        // establish a default destination address that can be used on subsequent send/ WSASend and recv/ WSARecv calls.
        // Any datagrams received from an address other than the destination address specified will be discarded.

        if ( connect( pThisUser ->sockUserUDP, reinterpret_cast <sockaddr*> (&pThisUser ->addrServer), sizeof(pThisUser ->addrServer) ) == SOCKET_ERROR )
        {
            pMainWindow ->printOutput( "Cannot start voice connection.\n"
                                      "NetworkService::setupVoiceConnection::connect() error: "
                                      + std::to_string(WSAGetLastError()),
                                      SilentMessageColor(false),
                                      true );

            closesocket(pThisUser ->sockUserUDP);

            return;
        }
        else
        {
            if ( sendVOIPReadyPacket() ) return;

            // Translate socket to non-blocking mode

            u_long arg = true;

            if ( ioctlsocket(pThisUser ->sockUserUDP, static_cast <long> (FIONBIO), &arg) == SOCKET_ERROR )
            {
                pMainWindow->printOutput( "Cannot start voice connection.\n"
                                          "NetworkService::setupVoiceConnection::ioctlsocket() error: "
                                          + std::to_string(WSAGetLastError()),
                                          SilentMessageColor(false),
                                          true );

                closesocket(pThisUser ->sockUserUDP);

                return;
            }
        }
    }
}

void NetworkService::serverMonitor()
{
    // Check if server died.
    // It should regularly send keep-alive message to us.

    do
    {
        clock_t timePassed        = clock() - lastTimeServerKeepAliveCame;
        float timePassedInSeconds = static_cast <float> (timePassed)/CLOCKS_PER_SEC;

        if ( timePassedInSeconds > (INTERVAL_KEEPALIVE_SEC * 3) )
        {
            lostConnection();
            return;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds(CHECK_IF_SERVER_DIED_EVERY_MS) );

    } while (bTextListen);
}

std::string NetworkService::formatAdressString(const std::string &sAdress)
{
    size_t iStartAdressStartPos = 0;

    for (size_t i = 0;   i < sAdress .size();   i++)
    {
        if ( sAdress[i] != ' ' )
        {
            iStartAdressStartPos = i;
            break;
        }
    }



    size_t iEndAdressStartPos = 0;

    for (size_t i = sAdress .size() - 1;   i > 0;   i--)
    {
        if ( sAdress[i] != ' ' )
        {
            iEndAdressStartPos = i;

            break;
        }
    }


    std::string sFormattedAdress = "";

    for (size_t i = iStartAdressStartPos;   i <= iEndAdressStartPos;   i++)
    {
        sFormattedAdress += sAdress[i];
    }



    return sFormattedAdress;
}

bool NetworkService::sendVOIPReadyPacket()
{
    // Send "I'm ready for VOIP" message to the server

    char firstMessage[MAX_NAME_LENGTH + 3];
    firstMessage[0] = UDP_SM_PREPARE;
    firstMessage[1] = static_cast <char> ( pThisUser ->sUserName .size() );
    std::memcpy( firstMessage + sizeof(firstMessage[0]) * 2, pThisUser ->sUserName .c_str(), pThisUser ->sUserName .size() );


    int iSentSize = sendto(pThisUser ->sockUserUDP, firstMessage, 2 + static_cast<int>(pThisUser ->sUserName .size()), 0,
                           reinterpret_cast <sockaddr*> (&pThisUser ->addrServer), sizeof(pThisUser ->addrServer));

    if ( iSentSize != static_cast <int> ( sizeof(firstMessage[0]) * 2 + pThisUser ->sUserName .size() ) )
    {
        if (iSentSize == SOCKET_ERROR)
        {
            pMainWindow ->printOutput( "Cannot start voice connection.\n"
                                      "NetworkService::setupVoiceConnection::sendto() error: "
                                      + std::to_string(WSAGetLastError()),
                                      SilentMessageColor(false),
                                      true );

            closesocket(pThisUser ->sockUserUDP);

            return true;
        }
        else
        {
            pMainWindow ->printOutput( "Cannot start voice connection.\n"
                                      "NetworkService::setupVoiceConnection::sendto() sent only: "
                                      + std::to_string(iSentSize) + " out of "
                                      + std::to_string(sizeof(firstMessage[0]) * 2 + pThisUser ->sUserName .size()),
                                      SilentMessageColor(false), true );

            closesocket(pThisUser ->sockUserUDP);

            return true;
        }
    }

    return false;
}

void NetworkService::listenTCPFromServer()
{
    char readBuffer[1];

    while(bTextListen)
    {
        while ( bTextListen && ( recv(pThisUser ->sockUserTCP, readBuffer, 0, 0) == 0 ) )
        {
            // There are some data to read

            mtxTCPRead .lock();

            // There are some data to receive
            int receivedAmount = recv(pThisUser ->sockUserTCP, readBuffer, 1, 0);
            if (receivedAmount == 0)
            {
                // Server sent FIN

                answerToFIN();
            }
            else
            {
                switch(readBuffer[0])
                {
                case(SM_NEW_USER):
                {
                    // We received info about the new user

                    receiveInfoAboutNewUser();

                    break;
                }
                case(SM_SOMEONE_DISCONNECTED):
                {
                    // Someone disconnected

                    deleteDisconnectedUserFromList();

                    break;
                }
                case(SM_CAN_START_UDP):
                {
                    std::thread listenVoiceThread (&NetworkService::listenUDPFromServer, this);
                    listenVoiceThread .detach();

                    break;
                }
                case(SM_SPAM_NOTICE):
                {
                    pMainWindow ->showMessageBox(true, "You can't send messages that quick.");

                    break;
                }
                case(SM_PING):
                {
                    // It's ping
                    receivePing();

                    break;
                }
                case(SM_KEEPALIVE):
                {
                    // This is keep-alive message
                    // We've been idle for INTERVAL_KEEPALIVE_SEC seconds
                    // We should answer in 10 seconds or will be disconnected

                    char keepAliveChar = 9;
                    send(pThisUser ->sockUserTCP, &keepAliveChar, 1, 0);

                    break;
                }
                case(SM_USERMESSAGE):
                {
                    // It's a message
                    receiveMessage();

                    break;
                }
                case(SM_KICKED):
                {
                    // We were kicked
                    pMainWindow ->printOutput("You were kicked by the server.", SilentMessageColor(false), true);

                    // Next message will be FIN
                    break;
                }
                case(SM_WRONG_PASSWORD_WAIT):
                {
                    pMainWindow->showMessageBox(true, "You must wait a couple of seconds after each incorrect password entry.");

                    break;
                }
                case(SM_GLOBAL_MESSAGE):
                {
                    receiveServerMessage();

                    break;
                }
                case(RC_CAN_ENTER_ROOM):
                {
                    canMoveToRoom();

                    break;
                }
                case(RC_USER_ENTERS_ROOM):
                {
                    userEntersRoom();

                    break;
                }
                case(RC_ROOM_IS_FULL):
                {
                    pMainWindow->showMessageBox(true, "The room is full.");

                    break;
                }
                case(RC_PASSWORD_REQ):
                {
                    char cRoomNameSize = 0;

                    recv(pThisUser->sockUserTCP, &cRoomNameSize, 1, 0);

                    char vNameBuffer[MAX_NAME_LENGTH + 1];
                    memset(vNameBuffer, 0, MAX_NAME_LENGTH + 1);

                    recv(pThisUser->sockUserTCP, vNameBuffer, cRoomNameSize, 0);

                    pMainWindow->showPasswordInputWindow(vNameBuffer);

                    break;
                }
                case(RC_WRONG_PASSWORD):
                {
                    pMainWindow->showMessageBox(true, "Wrong password.");

                    break;
                }
                case(RC_SERVER_MOVED_ROOM):
                {
                    serverMovedRoom();

                    break;
                }
                case(RC_SERVER_DELETES_ROOM):
                {
                    serverDeletesRoom();

                    break;
                }
                case(RC_SERVER_CREATES_ROOM):
                {
                    serverCreatesRoom();

                    break;
                }
                case(RC_SERVER_CHANGES_ROOM):
                {
                    serverChangesRoom();

                    break;
                }
                }

                lastTimeServerKeepAliveCame = clock();
            }

            mtxTCPRead .unlock();
        }

        if (bTextListen == false) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_TCP_MESSAGE_MS));
    }
}

void NetworkService::listenUDPFromServer()
{
    // Start the AudioService

    if ( pAudioService ->start() )
    {
        pMainWindow->printOutput( "Connected to the voice chat.\n",
                                  SilentMessageColor(false),
                                  true );
        bVoiceListen = true;
    }
    else
    {
        pMainWindow->printOutput( "An error occurred while starting the voice chat.\n",
                                  SilentMessageColor(false),
                                  true );
        return;
    }


    // Increase the send buffer size by 2, because we send a lot of data very fast.
    // This is how we try to avoid the 10035 error on sendto() (sometimes happens on old/slow systems).

    int iOptVal = 1;
    int iOptLen  = sizeof(iOptVal);

    int iReturnCode = getsockopt(pThisUser ->sockUserUDP, SOL_SOCKET, SO_SNDBUF, reinterpret_cast <char*> (&iOptVal), &iOptLen);
    if (iReturnCode == SOCKET_ERROR)
    {
        pMainWindow ->printOutput("NetworkService::listenUDPFromServer()::getsockopt (increase the send buffer size by 2) failed: "
                                 + std::to_string(WSAGetLastError())
                                 + ".\nSkipping this step.\n",
                                 SilentMessageColor(false),
                                 true);
    }
    else
    {
        iOptVal *= 2;

        iReturnCode = setsockopt(pThisUser ->sockUserUDP, SOL_SOCKET, SO_SNDBUF, reinterpret_cast <char*> (&iOptVal), iOptLen);
        if (iReturnCode == SOCKET_ERROR)
        {
            pMainWindow ->printOutput("NetworkService::listenUDPFromServer()::setsockopt (increase the send buffer size by 2) failed: "
                                     + std::to_string(WSAGetLastError())
                                     + ".\nSkipping this step.\n",
                                     SilentMessageColor(false),
                                     true);
        }
    }


    // Send "READY" for first ping check packet.

    char cReadyForPing = UDP_SM_USER_READY;
    int iSendSize = send(pThisUser ->sockUserUDP, &cReadyForPing, sizeof(cReadyForPing), 0);
    if (iSendSize != sizeof(cReadyForPing))
    {
        pMainWindow ->printOutput( "\nWARNING:\nNetworkService::listenUDPFromServer::sendto() (READY packet) failed and returned: "
                                   + std::to_string(WSAGetLastError()) + ".\n",
                                   SilentMessageColor(false),
                                   true);
    }


    // Listen to the server.

    char readBuffer[MAX_BUFFER_SIZE + 60];

    while (bVoiceListen)
    {
        int iSize = recv(pThisUser ->sockUserUDP, readBuffer, MAX_BUFFER_SIZE + 60, 0);

        while (iSize > 0)
        {
            mtxUDPRead .lock();

            if ( (readBuffer[0] == UDP_SM_PING || readBuffer[0] == UDP_SM_FIRST_PING) && (bVoiceListen) )
            {
                // it's ping check
                int iSentSize = send(pThisUser ->sockUserUDP, readBuffer, iSize, 0);
                if (iSentSize != iSize)
                {
                    if (iSize == SOCKET_ERROR)
                    {
                        pMainWindow ->printOutput( "\nWARNING:\nNetworkService::listenUDPFromServer::sendto() failed and returned: "
                                                   + std::to_string(WSAGetLastError()) + ".\n",
                                                   SilentMessageColor(false),
                                                   true);
                    }
                    else
                    {
                        pMainWindow ->printOutput( "\nWARNING:\nSomething went wrong and your ping check wasn't sent fully.\n",
                                                   SilentMessageColor(false),
                                                   true);
                    }
                }
            }
            else if (bVoiceListen)
            {
                // Copy user name.
                char userNameBuffer[MAX_NAME_LENGTH + 1];
                memset(userNameBuffer, 0, MAX_NAME_LENGTH + 1);

                std::memcpy(userNameBuffer, readBuffer + 1, static_cast <size_t> (readBuffer[0]));

                if ( readBuffer[ 1 + readBuffer[0] ] == 1 )
                {
                    // Last audio packet.

                    std::thread lastVoiceThread(&AudioService::playAudioData, pAudioService, nullptr,
                                                std::string(userNameBuffer), true);
                    lastVoiceThread .detach();
                }
                else
                {
                    // Not the last audio packet.


                    // Decrypt message.

                    unsigned short iEncryptedMessageSize = 0;

                    int iCurrentReadIndex = 1 + readBuffer[0] + 1;

                    std::memcpy(&iEncryptedMessageSize, readBuffer + iCurrentReadIndex, sizeof(iEncryptedMessageSize));
                    iCurrentReadIndex += sizeof(iEncryptedMessageSize);


                    char* pEncryptedMessageBytes = new char[iEncryptedMessageSize];
                    memset(pEncryptedMessageBytes, 0, iEncryptedMessageSize);

                    std::memcpy(pEncryptedMessageBytes, readBuffer + iCurrentReadIndex, iEncryptedMessageSize);

                    unsigned char* pDecryptedMessageBytes = pAES->DecryptECB(reinterpret_cast<unsigned char*>(pEncryptedMessageBytes), iEncryptedMessageSize,
                                                                             reinterpret_cast<unsigned char*>(vSecretAESKey));


                    short int* pAudio = new short int[ static_cast<size_t>(pAudioService ->getAudioPacketSizeInSamples()) ];
                    std::memcpy( pAudio, pDecryptedMessageBytes, static_cast<size_t>(pAudioService ->getAudioPacketSizeInSamples()) * 2 );

                    std::thread voiceThread(&AudioService::playAudioData, pAudioService, pAudio,
                                            std::string(userNameBuffer), false);
                    voiceThread .detach();

                    delete[] pEncryptedMessageBytes;
                    delete[] pDecryptedMessageBytes;
                }
            }

            mtxUDPRead .unlock();

            if (bVoiceListen)
            {
                iSize = recv(pThisUser ->sockUserUDP, readBuffer, MAX_BUFFER_SIZE, 0);
            }
            else
            {
                return;
            }
        }


        if (bVoiceListen)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_UDP_MESSAGE_MS));
        }
        else
        {
            return;
        }
    }
}

void NetworkService::receiveInfoAboutNewUser()
{
    // Prepare buffer.

    char readBuffer[MAX_NAME_LENGTH + 11];
    memset(readBuffer, 0, MAX_NAME_LENGTH + 11);




    // Read packet size.

    unsigned char iPacketSize = 0;
    recv(pThisUser ->sockUserTCP, readBuffer, 1, 0);
    std::memcpy(&iPacketSize, readBuffer, 1);




    // Read new online count.

    recv(pThisUser ->sockUserTCP, readBuffer, iPacketSize, 0);
    int iOnline = 0;
    std::memcpy(&iOnline, readBuffer, 4);



    mtxOtherUsers .lock();


    // Show on screen.

    pMainWindow   ->setOnlineUsersCount (iOnline);





    // Add new user.

    std::string sNewUserName = std::string(readBuffer + 5);

    User* pNewUser = new User( sNewUserName, 0, pMainWindow ->addNewUserToList(sNewUserName) );

    pAudioService ->setupUserAudio( pNewUser );

    if (pThisUser->pListWidgetItem->getRoom()->getIsWelcomeRoom())
    {
        pAudioService ->playConnectDisconnectSound(true);
    }

    vOtherUsers .push_back( pNewUser );



    mtxOtherUsers .unlock();



    // Show new user notice.

    if (pSettingsManager ->getCurrentSettings() ->bShowConnectDisconnectMessage)
    {
        pMainWindow  ->showUserConnectNotice(std::string(readBuffer + 5), SilentMessageColor(true));
    }
}

void NetworkService::receiveMessage()
{
    // Read packet size

    unsigned short int iPacketSize = 0;
    recv(pThisUser ->sockUserTCP, reinterpret_cast<char*>(&iPacketSize), 2, 0);

    char* pReadBuffer = new char[iPacketSize + 2];
    memset(pReadBuffer, 0, iPacketSize + 2);




    // Receive message

    int receivedAmount = recv(pThisUser ->sockUserTCP, pReadBuffer, iPacketSize, 0);

    // Message structure: "Hour:Minute. UserName: Message (message in wchar_t)".

    // Read time data:

    int iMessagePos  = 0;
    bool bFirstColonWas = false;
    for (int j = 0; j < receivedAmount; j++)
    {
        if ( (pReadBuffer[j] == ':') && (!bFirstColonWas) )
        {
            bFirstColonWas = true;
        }
        else if ( (pReadBuffer[j] == ':') && (bFirstColonWas) )
        {
            iMessagePos = j + 2;
            break;
        }
    }

    // Copy time info to 'timeText'
    // timeText = time info + user name
    // Max user name size = 20 + ~ max 7 chars before user name
    char timeText[MAX_NAME_LENGTH + 11];
    memset(timeText, 0, MAX_NAME_LENGTH + 11);
    std::memcpy(timeText, pReadBuffer, static_cast<unsigned long long>(iMessagePos));


    // Copy encrypted message size.

    unsigned short iEncryptedMessageSize = 0;
    std::memcpy(&iEncryptedMessageSize, pReadBuffer + iMessagePos, sizeof(iEncryptedMessageSize));


    // Decrypt message.

    char* pEncryptedMessageBytes = new char[iEncryptedMessageSize + 2];
    memset(pEncryptedMessageBytes, 0, iEncryptedMessageSize + 2);

    std::memcpy(pEncryptedMessageBytes, pReadBuffer + iMessagePos + sizeof(iEncryptedMessageSize), iEncryptedMessageSize);

    unsigned char* pDecryptedMessageBytes = pAES->DecryptECB(reinterpret_cast<unsigned char*>(pEncryptedMessageBytes), iEncryptedMessageSize,
                                                             reinterpret_cast<unsigned char*>(vSecretAESKey));



    // Show data on screen & play audio sound
    pMainWindow   ->printUserMessage    (std::string(timeText),
                                         std::wstring(reinterpret_cast<wchar_t*>(pDecryptedMessageBytes)), SilentMessageColor(true), true);
    pAudioService ->playNewMessageSound ();



    // Clear buffers.

    //delete[] pWCharText;
    delete[] pReadBuffer;
    delete[] pEncryptedMessageBytes;
    delete[] pDecryptedMessageBytes;
}

void NetworkService::deleteDisconnectedUserFromList()
{
    // Prepare buffer.

    char readBuffer[MAX_NAME_LENGTH + 15];
    memset(readBuffer, 0, MAX_NAME_LENGTH + 15);




    // Read disconnect type (lost or closed connection).

    char iDisconnectType = 0;
    recv(pThisUser ->sockUserTCP, &iDisconnectType, 1, 0);




    // Read packet size

    recv(pThisUser ->sockUserTCP, readBuffer, 1, 0);
    unsigned char iPacketSize = 0;
    std::memcpy(&iPacketSize, readBuffer, 1);




    // Read new online count

    recv(pThisUser ->sockUserTCP, readBuffer, iPacketSize, 0);
    int iOnline = 0;

    std::memcpy(&iOnline, readBuffer, 4);

    pMainWindow   ->setOnlineUsersCount (iOnline);




    std::string sDisconnectedUserName = std::string(readBuffer + 4);

    std::thread tEraseDisconnectedUser (&NetworkService::eraseDisconnectedUser, this, sDisconnectedUserName, iDisconnectType);
    tEraseDisconnectedUser .detach();
}

void NetworkService::receivePing()
{
    // Read packet size.

    unsigned short iPacketSize = 0;
    unsigned short iCurrentPos = 0;

    recv(pThisUser ->sockUserTCP, reinterpret_cast<char*>(&iPacketSize), sizeof(unsigned short), 0);

    do
    {
        // Read username size.

        char nameSize = 0;
        recv(pThisUser ->sockUserTCP, &nameSize, 1, 0);
        iCurrentPos++;




        // Read name.

        char nameBuffer[MAX_NAME_LENGTH + 1];
        memset(nameBuffer, 0, MAX_NAME_LENGTH + 1);

        recv(pThisUser ->sockUserTCP, nameBuffer, nameSize, 0);
        iCurrentPos += nameSize;




        // Read ping.

        unsigned short ping = 0;
        recv(pThisUser ->sockUserTCP, reinterpret_cast<char*>(&ping), sizeof(unsigned short), 0);
        iCurrentPos += sizeof(unsigned short);




        // Show on UI.

        std::string sUserName = std::string(nameBuffer);
        User* pUser           = nullptr;

        mtxOtherUsers .lock();

        if (sUserName == pThisUser ->sUserName)
        {
            pUser = pThisUser;
        }
        else
        {
            for (size_t i = 0;   i < vOtherUsers .size();   i++)
            {
                if (vOtherUsers[i] ->sUserName == sUserName)
                {
                    pUser = vOtherUsers[i];

                    break;
                }
            }
        }


        mtxOtherUsers .unlock();


        if (pUser)
        {
            // Show on screen.
            pUser       ->iPing = ping;
            pMainWindow ->setPingAndTalkingToUser(pUser ->pListWidgetItem, pUser ->iPing, pUser ->bTalking);
        }

    }while(iCurrentPos < iPacketSize);
}

void NetworkService::receiveServerMessage()
{
    unsigned short int iMessageSize = 0;
    recv(pThisUser->sockUserTCP, reinterpret_cast<char*>(&iMessageSize), sizeof(unsigned short), 0);

    char vMessageBuffer[MAX_BUFFER_SIZE];
    memset(vMessageBuffer, 0, MAX_BUFFER_SIZE);

    recv(pThisUser->sockUserTCP, vMessageBuffer, iMessageSize, 0);


    pMainWindow->showServerMessage(vMessageBuffer);


    pAudioService->playServerMessageSound();
}

void NetworkService::sendMessage(std::wstring message)
{
    if (message.length() * 2 > MAX_MESSAGE_LENGTH)
    {
        pMainWindow ->showMessageBox(true, "Your message is too big!");

        return;
    }



    // Encrypt message.

    char* pRawMessage = new char[message.length() * 2 + 1];
    memset(pRawMessage, 0, message.length() * 2 + 1);

    std::memcpy(pRawMessage, message.c_str(), message.length() * 2);

    unsigned int iEncryptedMessageSize = 0;
    unsigned char* pEncryptedMessageBytes = pAES->EncryptECB(reinterpret_cast<unsigned char*>(pRawMessage), static_cast<unsigned int>(message.length() * 2 + 1),
                                                             reinterpret_cast<unsigned char*>(vSecretAESKey), iEncryptedMessageSize);



    // Prepare send buffer.

    char* pSendBuffer = new char[ 3 + iEncryptedMessageSize + 2 ];
    memset(pSendBuffer, 0, 3 + iEncryptedMessageSize + 2);

    unsigned short int iPacketSize = static_cast <unsigned short> ( iEncryptedMessageSize );




    // Copy data to buffer.

    unsigned char commandType = SM_USERMESSAGE;

    std::memcpy( pSendBuffer,      &commandType,      1                  );
    std::memcpy( pSendBuffer + 1,  &iPacketSize,      2                  );
    std::memcpy( pSendBuffer + 3,  pEncryptedMessageBytes,  iEncryptedMessageSize );




    // Send buffer.

    int iSendBufferSize = static_cast <int> ( sizeof(commandType) + sizeof(iPacketSize) + iEncryptedMessageSize );

    int sendSize = send(  pThisUser ->sockUserTCP, pSendBuffer, iSendBufferSize, 0  );

    if ( sendSize != iSendBufferSize )
    {
        if (sendSize == SOCKET_ERROR)
        {
            int error = WSAGetLastError();

            if (error == 10054)
            {
                pMainWindow ->printOutput("\nWARNING:\nYour message has not been sent!\n"
                                         "NetworkService::sendMessage()::send() failed and returned: "
                                         + std::to_string(error) + ".",
                                         SilentMessageColor(false));
                lostConnection();
                pMainWindow ->clearTextEdit();
            }
            else
            {
                pMainWindow ->printOutput("\nWARNING:\nYour message has not been sent!\n"
                                         "NetworkService::sendMessage()::send() failed and returned: "
                                         + std::to_string(error) + ".\n",
                                         SilentMessageColor(false));
            }
        }
        else
        {
            pMainWindow ->printOutput("\nWARNING:\nWe could not send the whole message, because not enough "
                                     "space in the outgoing socket buffer.\n",
                                     SilentMessageColor(false));

            pMainWindow ->clearTextEdit();
        }
    }
    else
    {
        pMainWindow ->clearTextEdit();
    }

    delete[] pSendBuffer;
    delete[] pEncryptedMessageBytes;
    delete[] pRawMessage;
}

void NetworkService::enterRoom(std::string sName)
{
    if (bTextListen)
    {
        char vBuffer[MAX_NAME_LENGTH + 3];
        memset(vBuffer, 0, MAX_NAME_LENGTH + 3);

        vBuffer[0] = RC_ENTER_ROOM;
        vBuffer[1] = static_cast<char>(sName.size());

        std::memcpy(vBuffer + 2, sName.c_str(), sName.size());

        send(pThisUser->sockUserTCP, vBuffer, static_cast<int>(sName.size()) + 2, 0);
    }
}

void NetworkService::enterRoomWithPassword(std::string sRoomName, std::wstring sPassword)
{
    if (bTextListen)
    {
        char vBuffer[MAX_NAME_LENGTH * 3 + 5];
        memset(vBuffer, 0, MAX_NAME_LENGTH * 3 + 5);


        vBuffer[0] = RC_ENTER_ROOM_WITH_PASS;
        vBuffer[1] = static_cast<char>(sRoomName.size());

        int iCurrentIndex = 2;

        std::memcpy(vBuffer + iCurrentIndex, sRoomName.c_str(), sRoomName.size());
        iCurrentIndex += sRoomName.size();

        vBuffer[iCurrentIndex] = static_cast<char>(sPassword.size());
        iCurrentIndex++;

        std::memcpy(vBuffer + iCurrentIndex, sPassword.c_str(), sPassword.size() * 2);
        iCurrentIndex += sPassword.size() * 2;

        send(pThisUser->sockUserTCP, vBuffer, iCurrentIndex, 0);
    }
}

void NetworkService::sendVoiceMessage(char *pVoiceMessage, int iMessageSize, bool bLast)
{
    if (bVoiceListen)
    {
        int iSize = 0;

        if (bLast)
        {
            // '1' means it's the last voice packet.
            char lastVoice = 1;

            iSize = sendto( pThisUser ->sockUserUDP, &lastVoice, iMessageSize, 0,
                            reinterpret_cast <sockaddr*> (&pThisUser ->addrServer), sizeof(pThisUser ->addrServer) );
        }
        else
        {
            char vSend[MAX_BUFFER_SIZE + 70];
            memset(vSend, 0, MAX_BUFFER_SIZE + 70);

            // '2' means that it's not the last packet.
            vSend[0] = 2;



            // Encrypt voice message.

            unsigned int iEncryptedMessageSize = 0;
            unsigned char* pEncryptedMessageBytes = pAES->EncryptECB(reinterpret_cast<unsigned char*>(pVoiceMessage), static_cast<unsigned int>(iMessageSize),
                                                                     reinterpret_cast<unsigned char*>(vSecretAESKey), iEncryptedMessageSize);

            unsigned short iEncryptedDataSize = static_cast<unsigned short>(iEncryptedMessageSize);



            // Send to the server.

            std::memcpy(vSend + 1, &iEncryptedDataSize, sizeof(iEncryptedDataSize));
            std::memcpy(vSend + 1 + sizeof(iEncryptedDataSize), pEncryptedMessageBytes, iEncryptedDataSize);

            iMessageSize = 1 + sizeof(iEncryptedDataSize) + iEncryptedDataSize;

            iSize = sendto(pThisUser ->sockUserUDP, vSend, iMessageSize, 0,
                           reinterpret_cast<sockaddr*>(&pThisUser ->addrServer), sizeof(pThisUser ->addrServer));

            delete[] pEncryptedMessageBytes;
        }

        if (iSize != iMessageSize)
        {
            if (iSize == SOCKET_ERROR)
            {
                int iError = WSAGetLastError();

                if (iError == 10035)
                {
                    pMainWindow->printOutput("\nWARNING:\nYour voice message has not been sent!\n"
                                             "NetworkService::sendVoiceMessage()::sendto() failed and returned: "
                                             + std::to_string(iError) + " (send buffer is full).\n",
                                             SilentMessageColor(false),
                                             true);
                }
                else
                {
                    pMainWindow->printOutput("\nWARNING:\nYour voice message has not been sent!\n"
                                             "NetworkService::sendVoiceMessage()::sendto() failed and returned: "
                                             + std::to_string(iError) + ".\n",
                                             SilentMessageColor(false),
                                             true);
                }
            }
            else
            {
                pMainWindow->printOutput("\nWARNING:\nSomething went wrong and your voice message wasn't sent fully.\n",
                                         SilentMessageColor(false),
                                         true);
            }
        }
    }

    if (pVoiceMessage)
    {
        delete[] pVoiceMessage;
    }
}

void NetworkService::disconnect()
{
    if (bTextListen)
    {
        bTextListen  = false;


        if (bVoiceListen)
        {
            bVoiceListen = false;

            // Wait for listenUDPFromServer() to end.
            mtxUDPRead .lock();
            mtxUDPRead .unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_UDP_MESSAGE_MS));

            pAudioService ->stop();
        }


        // Wait for serverMonitor() to end.
        std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_IF_SERVER_DIED_EVERY_MS));


        // Wait for listenTCPFromServer() to end.
        mtxTCPRead .lock();
        mtxTCPRead .unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_TCP_MESSAGE_MS));


        // Translate socket to blocking mode

        u_long arg = false;
        if ( ioctlsocket(pThisUser ->sockUserTCP, static_cast <long> (FIONBIO), &arg) == SOCKET_ERROR )
        {
            pMainWindow ->printOutput("NetworkService::disconnect()::ioctsocket() (blocking mode) failed and returned: "
                                     + std::to_string(WSAGetLastError()) + ".\n",
                                     SilentMessageColor(false), true);
        }


        // Send FIN packet.

        char readBuffer[MAX_BUFFER_SIZE];

        int returnCode = shutdown(pThisUser ->sockUserTCP, SD_SEND);

        if (returnCode == SOCKET_ERROR)
        {
            pMainWindow ->printOutput("NetworkService::disconnect()::shutdown() function failed and returned: "
                                     + std::to_string(WSAGetLastError()) + ".\n",
                                     SilentMessageColor(false), true);
            closesocket(pThisUser ->sockUserTCP);
            closesocket(pThisUser ->sockUserUDP);
            WSACleanup();
            bWinSockLaunched = false;
        }
        else
        {
            size_t iAttemptCount = 0;

            for ( ; (recv(pThisUser ->sockUserTCP, readBuffer, MAX_BUFFER_SIZE, 0) != 0)
                    &&
                    (iAttemptCount < ATTEMPTS_TO_DISCONNECT_COUNT);
                    iAttemptCount++ )
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(INTERVAL_TCP_MESSAGE_MS));
            }

            if (iAttemptCount != ATTEMPTS_TO_DISCONNECT_COUNT)
            {
                returnCode = closesocket(pThisUser ->sockUserTCP);
                if (returnCode == SOCKET_ERROR)
                {
                    pMainWindow ->printOutput("NetworkService::disconnect()::closesocket() function failed and returned: "
                                             + std::to_string(WSAGetLastError()) + ".\n",
                                             SilentMessageColor(false), true);
                    WSACleanup();
                    bWinSockLaunched = false;
                }
                else
                {
                    pMainWindow ->printOutput("Connection closed successfully.\n",
                                             SilentMessageColor(false), true);

                    if (WSACleanup() == SOCKET_ERROR)
                    {
                        pMainWindow ->printOutput("NetworkService::disconnect()::WSACleanup() function failed and returned: "
                                                 + std::to_string(WSAGetLastError()) + ".\n",
                                                 SilentMessageColor(false), true);
                    }

                    // Delete user from UI.

                    pMainWindow ->deleteUserFromList        (nullptr, true);
                    pMainWindow ->setOnlineUsersCount       (0);
                    pMainWindow ->enableInteractiveElements (true, false);
                    pMainWindow ->setConnectDisconnectButton(true);

                    bWinSockLaunched = false;
                }
            }
            else
            {
                pMainWindow ->printOutput("Server has not responded.\n", SilentMessageColor(false), true);

                closesocket(pThisUser ->sockUserTCP);
                closesocket(pThisUser ->sockUserUDP);
                WSACleanup();
                bWinSockLaunched = false;



                // Delete user from UI.

                pMainWindow ->deleteUserFromList        (nullptr,true);
                pMainWindow ->setOnlineUsersCount       (0);
                pMainWindow ->enableInteractiveElements (true,false);
                pMainWindow ->setConnectDisconnectButton(true);
            }
        }


        cleanUp();


        pMainWindow ->clearTextEdit();
    }
    else
    {
        pMainWindow ->showMessageBox(true, "You are not connected." );
    }
}

void NetworkService::lostConnection()
{
    pMainWindow->printOutput( "\nThe server is not responding...\n", SilentMessageColor(false), true );

    bTextListen  = false;

    if (bVoiceListen)
    {
        bVoiceListen = false;
        closesocket(pThisUser ->sockUserUDP);
        pAudioService ->playLostConnectionSound();
        pAudioService ->stop();
    }

    closesocket(pThisUser ->sockUserTCP);
    WSACleanup();
    bWinSockLaunched = false;


    cleanUp();



    // Delete user from UI.

    pMainWindow ->deleteUserFromList(nullptr, true);
    pMainWindow ->setOnlineUsersCount(0);
    pMainWindow ->enableInteractiveElements(true, false);
    pMainWindow ->setConnectDisconnectButton(true);
}

void NetworkService::answerToFIN()
{
    bTextListen = false;

    if (bVoiceListen)
    {
        pAudioService ->stop();

        bVoiceListen = false;

        std::this_thread::sleep_for( std::chrono::milliseconds(INTERVAL_UDP_MESSAGE_MS) );

        closesocket(pThisUser ->sockUserUDP);
    }

    // Wait for serverMonitor() to end
    std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_IF_SERVER_DIED_EVERY_MS));


    pMainWindow->printOutput("Server is closing connection.\n", SilentMessageColor(false), true);

    int returnCode = shutdown(pThisUser ->sockUserTCP, SD_SEND);

    if (returnCode == SOCKET_ERROR)
    {
         pMainWindow->printOutput("NetworkService::listenForServer()::shutdown() function failed and returned: "
                                  + std::to_string(WSAGetLastError()) + ".\n",
                                  SilentMessageColor(false),
                                  true);

         closesocket(pThisUser ->sockUserTCP);
         WSACleanup();
         bWinSockLaunched = false;
    }
    else
    {
        returnCode = closesocket(pThisUser ->sockUserTCP);
        if (returnCode == SOCKET_ERROR)
        {
            pMainWindow->printOutput("NetworkService::listenForServer()::closesocket() function failed and returned: "
                                     + std::to_string(WSAGetLastError()) + ".\n",
                                     SilentMessageColor(false),
                                     true);

            WSACleanup();
            bWinSockLaunched = false;
        }
        else
        {
            if (WSACleanup() == SOCKET_ERROR)
            {
                pMainWindow->printOutput("NetworkService::listenForServer()::WSACleanup() function failed and returned: "
                                         + std::to_string(WSAGetLastError()) + ".\n",
                                         SilentMessageColor(false),
                                         true);
            }
            else
            {
                pMainWindow->setOnlineUsersCount(0);
                pMainWindow->enableInteractiveElements(true, false);
                pMainWindow ->setConnectDisconnectButton(true);

                bWinSockLaunched = false;

                pMainWindow->printOutput("Connection closed successfully.\n",
                                         SilentMessageColor(false), true);

                pMainWindow->deleteUserFromList(nullptr, true);
            }
        }
    }


    cleanUp();


    pMainWindow ->clearTextEdit();
}





void NetworkService::stop()
{
    if (bTextListen)
    {
        disconnect();
    }
}

void NetworkService::cleanUp()
{
    mtxOtherUsers .lock();


    for (size_t i = 0;   i < vOtherUsers .size();   i++)
    {
        vOtherUsers[i] ->mtxUser .lock();
        vOtherUsers[i] ->mtxUser .unlock();

        delete vOtherUsers[i];
        vOtherUsers[i] = nullptr;
    }

    vOtherUsers .clear();


    if (pThisUser)
    {
        delete pThisUser;
        pThisUser = nullptr;
    }


    mtxOtherUsers .unlock();
}

void NetworkService::canMoveToRoom()
{
    char vBuffer[MAX_NAME_LENGTH + 1];
    memset(vBuffer, 0, MAX_NAME_LENGTH + 1);

    char cRoomNameSize = 0;

    recv(pThisUser->sockUserTCP, &cRoomNameSize, 1, 0);

    recv(pThisUser->sockUserTCP, vBuffer, cRoomNameSize, 0);

    mtxRooms.lock();

    pMainWindow->moveUserToRoom(pThisUser->pListWidgetItem, vBuffer);

    mtxRooms.unlock();
}

void NetworkService::userEntersRoom()
{
    char vBuffer[MAX_NAME_LENGTH + 1];
    memset(vBuffer, 0, MAX_NAME_LENGTH + 1);

    char cNameSize = 0;

    recv(pThisUser->sockUserTCP, &cNameSize, 1, 0);

    recv(pThisUser->sockUserTCP, vBuffer, cNameSize, 0);

    std::string sUserName(vBuffer);
    memset(vBuffer, 0, MAX_NAME_LENGTH + 1);

    char cRoomNameSize = 0;

    recv(pThisUser->sockUserTCP, &cRoomNameSize, 1, 0);

    recv(pThisUser->sockUserTCP, vBuffer, cRoomNameSize, 0);

    std::string sRoomName(vBuffer);

    std::string sOldRoom = "";
    std::string sOurRoom = "";

    mtxOtherUsers.lock();

    for (size_t i = 0; i < vOtherUsers.size(); i++)
    {
        if (vOtherUsers[i]->sUserName == sUserName)
        {
            mtxRooms.lock();

            sOurRoom = pThisUser->pListWidgetItem->getRoom()->getRoomName().toStdString();
            sOldRoom             = vOtherUsers[i]->pListWidgetItem->getRoom()->getRoomName().toStdString();

            pMainWindow->moveUserToRoom(vOtherUsers[i]->pListWidgetItem, sRoomName);

            mtxRooms.unlock();

            break;
        }
    }

    mtxOtherUsers.unlock();

    if (sOurRoom == sOldRoom)
    {
        pAudioService ->playConnectDisconnectSound(false);
    }
    else if (sOurRoom == sRoomName)
    {
        pAudioService ->playConnectDisconnectSound(true);
    }
}

void NetworkService::serverMovedRoom()
{
    char cRoomNameSize = 0;
    recv(pThisUser->sockUserTCP, &cRoomNameSize, 1, 0);

    char vRoomNameBuffer[MAX_NAME_LENGTH + 1];
    memset(vRoomNameBuffer, 0, MAX_NAME_LENGTH + 1);

    recv(pThisUser->sockUserTCP, vRoomNameBuffer, cRoomNameSize, 0);

    char cMoveUp = 0;
    recv(pThisUser->sockUserTCP, &cMoveUp, 1, 0);

    mtxRooms.lock();
    mtxOtherUsers.lock();

    pMainWindow->moveRoom(vRoomNameBuffer, cMoveUp);

    mtxOtherUsers.unlock();
    mtxRooms.unlock();
}

void NetworkService::serverDeletesRoom()
{
    char cRoomNameSize = 0;
    recv(pThisUser->sockUserTCP, &cRoomNameSize, 1, 0);

    char vRoomNameBuffer[MAX_NAME_LENGTH + 1];
    memset(vRoomNameBuffer, 0, MAX_NAME_LENGTH + 1);

    recv(pThisUser->sockUserTCP, vRoomNameBuffer, cRoomNameSize, 0);

    mtxRooms.lock();
    mtxOtherUsers.lock();

    pMainWindow->deleteRoom(vRoomNameBuffer);

    mtxOtherUsers.unlock();
    mtxRooms.unlock();
}

void NetworkService::serverCreatesRoom()
{
    char cRoomNameSize = 0;
    recv(pThisUser->sockUserTCP, &cRoomNameSize, 1, 0);

    char vRoomNameBuffer[MAX_NAME_LENGTH + 1];
    memset(vRoomNameBuffer, 0, MAX_NAME_LENGTH + 1);

    recv(pThisUser->sockUserTCP, vRoomNameBuffer, cRoomNameSize, 0);


    unsigned int iMaxUsers = 0;
    recv(pThisUser->sockUserTCP, reinterpret_cast<char*>(&iMaxUsers), sizeof(unsigned int), 0);



    mtxRooms.lock();
    mtxOtherUsers.lock();

    pMainWindow->createRoom(vRoomNameBuffer, u"", iMaxUsers);

    mtxOtherUsers.unlock();
    mtxRooms.unlock();
}

void NetworkService::serverChangesRoom()
{
    char cOldRoomName = 0;
    recv(pThisUser->sockUserTCP, &cOldRoomName, 1, 0);

    char vOldRoomNameBuffer[MAX_NAME_LENGTH + 1];
    memset(vOldRoomNameBuffer, 0, MAX_NAME_LENGTH + 1);

    recv(pThisUser->sockUserTCP, vOldRoomNameBuffer, cOldRoomName, 0);



    char cRoomName = 0;
    recv(pThisUser->sockUserTCP, &cRoomName, 1, 0);

    char vRoomNameBuffer[MAX_NAME_LENGTH + 1];
    memset(vRoomNameBuffer, 0, MAX_NAME_LENGTH + 1);

    recv(pThisUser->sockUserTCP, vRoomNameBuffer, cRoomName, 0);



    unsigned int iMaxUsers = 0;
    recv(pThisUser->sockUserTCP, reinterpret_cast<char*>(&iMaxUsers), sizeof(unsigned int), 0);



    mtxRooms.lock();
    mtxOtherUsers.lock();

    pMainWindow->changeRoomSettings(vOldRoomNameBuffer, vRoomNameBuffer, iMaxUsers);

    mtxOtherUsers.unlock();
    mtxRooms.unlock();
}
