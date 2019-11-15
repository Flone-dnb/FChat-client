﻿#pragma once


// STL
#include <string>


class SettingsFile
{

public:

    SettingsFile(int iPushToTalkButton = 0x54              /* T button */,
                 unsigned short int iMasterVolume = 52428  /* ~80% */,
                 const std::string& sUserName = "")
    {
        this ->iPushToTalkButton = iPushToTalkButton;
        this ->iMasterVolume     = iMasterVolume;
        this ->sUsername         = sUserName;
    }


    std::string getPushToTalkButtonName() const
    {
        std::string sButtonText = "";


        if ( (iPushToTalkButton >= 0x41)
             &&
             (iPushToTalkButton <= 0x5a) )
        {
            // From A to Z
            sButtonText += static_cast <char> (iPushToTalkButton);
        }
        else if ( iPushToTalkButton == 0x12 )
        {
            // Alt
            sButtonText += "Alt";
        }
        else if ( iPushToTalkButton == 0x05 )
        {
            // X1 (mouse button)
            sButtonText += "X1";
        }
        else if ( iPushToTalkButton == 0x06 )
        {
            // X2 (mouse button)
            sButtonText += "X2";
        }


        return sButtonText;
    }


    // ------------------------------------------------------------


    std::string        sUsername;


    int                iPushToTalkButton;
    unsigned short int iMasterVolume;
};
