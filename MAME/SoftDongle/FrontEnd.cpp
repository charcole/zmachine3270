#include "FrontEnd.h"

const char* SelectionScreen[24] =
{
"                            Charlie Cole Presents...                          \n",
"          ##          ##                               ##    ##   ####\n",
"          #          #                                #  #  #  #  #  #\n",
"          #  ####    ##   ##  # # ######    #  # #       # #   #    #\n",
"          #   # #    #   #  # ##  ## # #   ### ##      ##  #   #   #\n",
"          #  #  #    #   #  # #   #  # #  ##   #      #  # #  #   #\n",
"         ##  #  ##   #    ##  #   # #  ##  ### #     ####   ##   ##\n",
"                    #\n",
"                   ##\n",
"                                    meets\n",
"\n",
"      ::::::::::       ::::::::       :::::::::       ::::::::        ::::::::\n",
"     :+:             :+:    :+:      :+:    :+:     :+:    :+:      :+:    :+:\n",
"    +:+             +:+             +:+    +:+            +:+            +:+\n",
"   +#++:++#        +#++:++#++      +#++:++#+          +#++:           +#+\n",
"  +#+                    +#+      +#+                   +#+        +#+\n",
" #+#             #+#    #+#      #+#            #+#    #+#       #+#\n",
"##########       ########       ###             ########       ##########\n",
"\n",
"                Infocom Games                Internet\n",
"                 (1)Planetfall                (T)N3270 Client\n",
"                 (2)Trinity                   (S)SH Client\n",
"                 (3)Sherlock:TRotCJ           (W)ikipedia\n",
"                              Enter Choice:"
};

int FrontEnd::Show(Screen* CurrentScreen)
{
    while (true)
    {
        CurrentScreen->SetCursorPosition(0,0);
        for (int Line = 0; Line < 24; Line++)
        {
            CurrentScreen->Print(SelectionScreen[Line]);
        }
        
        char Selection[32];
        CurrentScreen->ReadInput(Selection, sizeof(Selection));

        switch (Selection[0])
        {
            case '1':
            case '2':
            case '3':
            case '4': // secret game
                return Selection[0] - '1';
            case 'T':
            case 't':
                return -1;
            case 'W':
            case 'w':
                return -2;
            case 'S':
            case 's':
                return -3;
        }
    }
    return -1;
}