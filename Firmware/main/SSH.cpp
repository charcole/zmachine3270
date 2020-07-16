#include "SSH.h"
#include "Screen.h"

#include "libssh2.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include <stdlib.h>
#include <stdio.h>

static int waitsocket(int socket_fd, LIBSSH2_SESSION *session)
{
    struct timeval timeout;
    int rc;
    fd_set fd;
    fd_set *writefd = NULL;
    fd_set *readfd = NULL;
    int dir;
 
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
 
    FD_ZERO(&fd);
 
    FD_SET(socket_fd, &fd);
 
    /* now make sure we wait in the correct direction */ 
    dir = libssh2_session_block_directions(session);

 
    if(dir & LIBSSH2_SESSION_BLOCK_INBOUND)
        readfd = &fd;
 
    if(dir & LIBSSH2_SESSION_BLOCK_OUTBOUND)
        writefd = &fd;
 
    rc = select(socket_fd + 1, readfd, writefd, NULL, &timeout);
 
    return rc;
}

void SSH::Run()
{
    char Address[80];
    char *HostName = nullptr, *PortNum = nullptr, *UserName = nullptr;
    GScreen.Clear();
    GScreen.SetCursorPosition(1 , 0);
    GScreen.Print("                                     SSH Client");
    GScreen.SetCursorPosition(0, 2);
    while(!HostName)
    {
        GScreen.Print("ADDRESS (username@ip:port) :");
        GScreen.ReadInput(Address, sizeof(Address));

        UserName = Address;
        HostName = strchr(Address, '@');
        if (HostName)
            *(HostName++) = '\0';
        PortNum = strchr(HostName, ':');
        if (PortNum)
            *(PortNum++) = '\0';
    }

    char Password[80];
    GScreen.Print("PASSWORD :");
    GScreen.ReadInput(Password, sizeof(Password), false, -1, true);

    int SocketFD = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (SocketFD == -1)
    {
        printf("Failed to create socket\n");
        return;
    }
    
    int Port = PortNum ? atoi(PortNum) : 22;

    struct sockaddr_in DestAddr;
    DestAddr.sin_addr.s_addr = inet_addr(HostName);
    DestAddr.sin_family = AF_INET;
    DestAddr.sin_port = htons(Port);

    if (connect(SocketFD, (struct sockaddr *)&DestAddr, sizeof(DestAddr)) == -1)
    {
        close(SocketFD);
        printf("Failed to connect socket\n");
        return;
    }

    printf("Connected\n");

    libssh2_init(0);

    LIBSSH2_SESSION* session = libssh2_session_init();
    if (!session)
    {
        printf("Couldn't create session\n");
        return;
    }
    
    //libssh2_trace(session, ~0u);

    libssh2_session_set_blocking(session, 0);

    int rc;
    while ((rc = libssh2_session_handshake(session, SocketFD)) == LIBSSH2_ERROR_EAGAIN)
    {
        vTaskDelay(1);
    }
    if (rc)
    {
        return;
    }

    LIBSSH2_KNOWNHOSTS* nh = libssh2_knownhost_init(session);
    if (!nh)
    {
        printf("Couldn't create known hosts\n");
        return;
    }

    //libssh2_knownhost_readfile(nh, "known_hosts", LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    //libssh2_knownhost_writefile(nh, "dumpfile", LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t len;
    int type;
    const char* fingerprint = libssh2_session_hostkey(session, &len, &type);

    if (fingerprint)
    {
        struct libssh2_knownhost *host;
        int check = libssh2_knownhost_checkp(nh, HostName, Port,
                                             fingerprint, len,
                                             LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                                             LIBSSH2_KNOWNHOST_KEYENC_RAW,
                                             &host);
        (void)check;
        //printf("Host check: %d, key: %s\n", check,
        //        (check <= LIBSSH2_KNOWNHOST_CHECK_MISMATCH) ? host->key : "<none>");
    }
    else
    {
        printf("No finger print");
        return;
    }
    libssh2_knownhost_free(nh);

    /* We could authenticate via password */
    while ((rc = libssh2_userauth_password(session, UserName, Password)) == LIBSSH2_ERROR_EAGAIN)
    {
        vTaskDelay(1);
    }
    if (rc)
    {
        printf("Authentication by password failed.\n");

        libssh2_session_disconnect(session, "Normal Shutdown, Thank you for playing");
        libssh2_session_free(session);
        close(SocketFD);
        printf("all done\n");
        libssh2_exit();
        return;
    }

    LIBSSH2_CHANNEL *channel;
    while ((channel = libssh2_channel_open_session(session)) == NULL &&
           libssh2_session_last_error(session, NULL, NULL, 0) == LIBSSH2_ERROR_EAGAIN)
    {
        waitsocket(SocketFD, session);
    }
    if (channel == NULL)
    {
        printf("Channel open error\n");
        exit(1);
    }

    if(libssh2_channel_request_pty(channel, "vanilla") < 0)
    {
        // Seems to fail but does work as we have a pseudo terminal
        //GScreen.Print("Failed requesting PTY\n");
    }

    while ((rc = libssh2_channel_shell(channel)) == LIBSSH2_ERROR_EAGAIN)
    {
        waitsocket(SocketFD, session);
    }
    if (rc != 0)
    {
        printf("Exec error\n");
        exit(1);
    }

    bool bError = false;
    int bytecount = 0;
    while (!bError && !libssh2_channel_eof(channel))
    {
        int rc;
        char buffer[1024];

        bool bHasCancelledInput = false;
        while (true)
        {
            int InputLength = GScreen.ReadInput((char *)buffer, sizeof(buffer), false, 20, false, false);
            if (InputLength > 0)
            {
                printf("Got some input\n");
                buffer[InputLength++] = '\n'; // Add implicit return
                while ((rc = libssh2_channel_write(channel, buffer, InputLength)) == LIBSSH2_ERROR_EAGAIN)
                {
                    vTaskDelay(10);
                }
                bError |= (rc < 0);
                break;
            }
            else if (InputLength == 0)
            {
                printf("Has cancelled the input\n");
                break;
            }
            else if (InputLength == -1 && !bHasCancelledInput)
            {
                struct timeval timeout;
                fd_set fd;
                timeout.tv_sec = 0;
                timeout.tv_usec = 0;
                FD_ZERO(&fd);
                FD_SET(SocketFD, &fd);
                if (select(SocketFD + 1, &fd, NULL, NULL, &timeout) > 0 && FD_ISSET(SocketFD, &fd))
                {
                    printf("Cancelling input as we have some more data pending\n");
                    GScreen.CancelInput();
                    bHasCancelledInput = true;
                }
            }
        }

        for (int Pass = 0; Pass < 2; Pass++)
        {
            do
            {
                if (Pass == 0)
                    rc = libssh2_channel_read(channel, buffer, sizeof(buffer));
                else
                    rc = libssh2_channel_read_stderr(channel, buffer, sizeof(buffer));

                if (rc > 0)
                {
                    int i;
                    bytecount += rc;
                    for (i = 0; i < rc; ++i)
                        GScreen.Print(buffer[i]);
                }
                else if (rc < 0)
                {
                    if (rc != LIBSSH2_ERROR_EAGAIN)
                    {
                        /* no need to output this for the EAGAIN case */
                        printf("libssh2_channel_read returned %d\n", rc);
                        bError = true;
                    }
                }
            } while (rc > 0);
        }
    }

    int exitcode = 127;
    char *exitsignal = (char *)"none";
    while ((rc = libssh2_channel_close(channel)) == LIBSSH2_ERROR_EAGAIN)
        waitsocket(SocketFD, session);
    if (rc == 0)
    {
        exitcode = libssh2_channel_get_exit_status(channel);
        libssh2_channel_get_exit_signal(channel, &exitsignal, NULL, NULL, NULL, NULL, NULL);
    }

    char ExitMessage[80];
    if (exitsignal)
        sprintf(ExitMessage, "\nGot signal: %s\n", exitsignal);
    else
        sprintf(ExitMessage, "\nEXIT: %d bytecount: %d\n", exitcode, bytecount);
    GScreen.Print(ExitMessage);

    libssh2_channel_free(channel);
    channel = NULL;

    libssh2_session_disconnect(session, "Normal Shutdown, Thank you for playing");
    libssh2_session_free(session);

    close(SocketFD);
    printf("all done\n");

    libssh2_exit();

    GScreen.Print("\nEnter to exit");
    GScreen.ReadInput(ExitMessage, sizeof(ExitMessage));
}