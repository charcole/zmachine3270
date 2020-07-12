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
"                 (P)lanetfall                 (T)N3270 Client]\n",
"                 (L)urking Horror             (S)SH Client]\n",
"                 (H)itchhiker's Guide         (W)ikipedia]\n",
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
            case 'P':
            case 'p':
                return 0;
            case 'L':
            case 'l':
                return 2;
            case 'H':
            case 'h':
                return 1;
            case 'T':
            case 't':
                return -1;
            case 'W':
            case 'w':
                return -2;
        }
    }
    return -1;
}