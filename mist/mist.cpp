#define _CRT_RAND_S
#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <Windows.h>
#include <WinSock2.h>
#include <Ws2tcpip.h>
#include <stdio.h>
#include <assert.h>
#include <shellapi.h>
#include <objbase.h>

#pragma comment(lib, "miniupnpc.lib")
#pragma comment(lib, "libnatpmp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define MINIUPNP_STATICLIB
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#define NATPMP_STATICLIB
#include <natpmp.h>

#define STUN_MESSAGE_BINDING_REQUEST 0x0001
#define STUN_MESSAGE_BINDING_SUCCESS 0x0101
#define STUN_MESSAGE_COOKIE 0x2112a442

#define STUN_ATTRIBUTE_MAPPED_ADDRESS 0x0001
#define STUN_ATTRIBUTE_XOR_MAPPED_ADDRESS 0x0020

typedef struct _STUN_MAPPED_IPV4_ADDRESS_ATTRIBUTE {
    USHORT attributeType;
    USHORT attributeLength;
    UCHAR reserved;
    UCHAR addressFamily;
    USHORT port;
    ULONG address;
} STUN_MAPPED_IPV4_ADDRESS_ATTRIBUTE, *PSTUN_MAPPED_IPV4_ADDRESS_ATTRIBUTE;

typedef struct _STUN_MESSAGE {
    USHORT messageType;
    USHORT messageLength;
    UINT magicCookie;
    UINT transactionId[3];
} STUN_MESSAGE, *PSTUN_MESSAGE;

static struct port_entry {
    int proto;
    int port;
    bool withServer;
} k_Ports[] = {
    {IPPROTO_TCP, 47984, false},
    {IPPROTO_TCP, 47989, false},
    {IPPROTO_TCP, 48010, true},
    {IPPROTO_UDP, 47998, true},
    {IPPROTO_UDP, 47999, true},
    {IPPROTO_UDP, 48000, true},
    {IPPROTO_UDP, 48002, true},
    {IPPROTO_UDP, 48010, true}
};

char logFilePath[MAX_PATH + 1];

void DisplayMessage(const char* message, bool error = true)
{
    printf("%s\n", message);
    printf("--------------- MISS LOG -------------------\n");

    char missPath[MAX_PATH + 1];
    ExpandEnvironmentStringsA("%ProgramData%\\MISS\\miss-current.log", missPath, sizeof(missPath));
    FILE* f = fopen(missPath, "r");
    if (f != nullptr) {
        char buffer[1024];
        while (!feof(f)) {
            int bytesRead = fread(buffer, 1, ARRAYSIZE(buffer), f);
            fwrite(buffer, 1, bytesRead, stdout);
        }
        fclose(f);
    }
    else {
        printf("Failed to find MISS log\n");
    }

    fflush(stdout);

    DWORD flags = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
    flags |= error ? MB_ICONERROR : MB_ICONINFORMATION;
    MessageBoxA(nullptr, message, "Moonlight Internet Streaming Tester", flags);

    if (error) {
        flags = MB_YESNO | MB_TOPMOST | MB_SETFOREGROUND | MB_ICONINFORMATION;
        switch (MessageBoxA(nullptr, "Would you like to view the troubleshooting log?",
            "Moonlight Internet Streaming Tester", flags))
        {
        case IDYES:
            // It's recommended to initialize COM before calling ShellExecute()
            CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            ShellExecuteA(nullptr, "open", logFilePath, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
    }
}

bool IsGameStreamEnabled()
{
    DWORD error;
    DWORD enabled;
    DWORD len;
    HKEY key;

    error = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\NVIDIA Corporation\\NvStream", 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (error != ERROR_SUCCESS) {
        printf("RegOpenKeyEx() failed: %d\n", error);
        DisplayMessage("GeForce Experience is not installed. Please install GeForce Experience to use Moonlight.");
        return false;
    }

    len = sizeof(enabled);
    error = RegQueryValueExA(key, "EnableStreaming", nullptr, nullptr, (LPBYTE)&enabled, &len);
    RegCloseKey(key);
    if (error != ERROR_SUCCESS) {
        printf("RegQueryValueExA() failed: %d\n", error);
        DisplayMessage("GeForce Experience is not installed. Please install GeForce Experience to use Moonlight.");
        return false;
    }
    else if (!enabled) {
        DisplayMessage("GameStream is not enabled in GeForce Experience. Please open GeForce Experience settings, navigate to the Shield tab, and turn GameStream on.");
        return false;
    }
    else {
        printf("GeForce Experience installed and GameStream is enabled\n");
        return true;
    }
}

enum PortTestStatus {
    PortTestOk,
    PortTestError,
    PortTestUnknown
};
PortTestStatus TestPort(PSOCKADDR_STORAGE addr, int proto, int port, bool withServer)
{
    SOCKET clientSock = INVALID_SOCKET, serverSock = INVALID_SOCKET;
    int err;

    clientSock = socket(addr->ss_family, proto == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, proto);
    if (clientSock == INVALID_SOCKET) {
        printf("socket() failed: %d\n", WSAGetLastError());
        return PortTestError;
    }

    if (withServer) {
        serverSock = socket(addr->ss_family, proto == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, proto);
        if (serverSock == INVALID_SOCKET) {
            printf("socket() failed: %d\n", WSAGetLastError());
            closesocket(clientSock);
            return PortTestError;
        }

        SOCKADDR_IN sin = {};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        err = bind(serverSock, (struct sockaddr*)&sin, sizeof(sin));
        if (err == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEADDRINUSE) {
                // If someone is already listening (perhaps GFE is currently streaming),
                // we can proceed if it's a TCP connection.
                if (proto == IPPROTO_TCP) {
                    closesocket(serverSock);
                    serverSock = INVALID_SOCKET;
                }
                else {
                    // We can't continue to test for UDP ports.
                    printf("Unknown (in use)\n");
                    closesocket(clientSock);
                    closesocket(serverSock);
                    return PortTestUnknown;
                }
            }
            else {
                printf("bind() failed: %d\n", WSAGetLastError());
                closesocket(clientSock);
                closesocket(serverSock);
                return PortTestError;
            }
        }

        if (proto == IPPROTO_TCP && serverSock != INVALID_SOCKET) {
            err = listen(serverSock, 1);
            if (err == SOCKET_ERROR) {
                printf("listen() failed: %d\n", WSAGetLastError());
                closesocket(clientSock);
                closesocket(serverSock);
                return PortTestError;
            }
        }
    }

    ULONG nbIo = 1;
    err = ioctlsocket(clientSock, FIONBIO, &nbIo);
    if (err == SOCKET_ERROR) {
        printf("ioctlsocket() failed: %d\n", WSAGetLastError());
        closesocket(clientSock);
        if (serverSock != INVALID_SOCKET) {
            closesocket(serverSock);
        }
        return PortTestError;
    }

    SOCKADDR_IN6 sin6;
    int addrLen = addr->ss_family == AF_INET ?
        sizeof(SOCKADDR_IN) : sizeof(SOCKADDR_IN6);

    RtlCopyMemory(&sin6, addr, addrLen);
    sin6.sin6_port = htons(port);

    if (proto == IPPROTO_TCP) {
        err = connect(clientSock, (struct sockaddr*)&sin6, addrLen);
        if (err == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            printf("connect() failed: %d\n", WSAGetLastError());
        }
        else {
            struct timeval timeout = {};
            fd_set fds;

            FD_ZERO(&fds);
            FD_SET(clientSock, &fds);

            timeout.tv_sec = 3;
            err = select(0, nullptr, &fds, nullptr, &timeout);
            if (err == 1) {
                // Our FD was signalled for connect() completion
                printf("Success\n");
            }
            else if (err == 0) {
                // Timed out
                printf("Timeout\n");
            }
            else {
                printf("select() failed: %d\n", WSAGetLastError());
            }
        }

        closesocket(clientSock);
        if (serverSock != INVALID_SOCKET) {
            closesocket(serverSock);
        }

        return err == 1 ? PortTestOk : PortTestError;
    }
    else {
        const char testMsg[] = "mist-test";
        err = sendto(clientSock, testMsg, sizeof(testMsg), 0, (struct sockaddr*)&sin6, addrLen);
        if (err == SOCKET_ERROR) {
            printf("sendto() failed: %d\n", WSAGetLastError());
            closesocket(clientSock);
            closesocket(serverSock);
            return PortTestError;
        }

        struct timeval timeout = {};
        fd_set fds;

        FD_ZERO(&fds);
        FD_SET(serverSock, &fds);

        timeout.tv_sec = 3;
        err = select(0, &fds, nullptr, nullptr, &timeout);
        if (err == 1) {
            // Our FD was signalled for data available
            printf("Success\n");
        }
        else if (err == 0) {
            // Timed out
            printf("Timeout\n");
        }
        else {
            printf("select() failed: %d\n", WSAGetLastError());
        }

        closesocket(clientSock);
        closesocket(serverSock);

        return err == 1 ? PortTestOk : PortTestError;
    }
}

bool TestAllPorts(PSOCKADDR_STORAGE addr, const char* baseMessage, char* message, int messageLength)
{
    strcpy_s(message, messageLength, baseMessage);
    message += strlen(baseMessage);
    messageLength -= strlen(baseMessage);

    bool ret = true;
    for (int i = 0; i < ARRAYSIZE(k_Ports); i++) {
        printf("Testing %s %d...",
            k_Ports[i].proto == IPPROTO_TCP ? "TCP" : "UDP",
            k_Ports[i].port);
        PortTestStatus status = TestPort(addr, k_Ports[i].proto, k_Ports[i].port, k_Ports[i].withServer);
        if (status != PortTestOk) {
            // If we got an unknown result, assume it matches with whatever
            // we've gotten so far.
            if (status == PortTestError || !ret) {
                int msgLen = snprintf(message, messageLength, "%s %d\n",
                    k_Ports[i].proto == IPPROTO_TCP ? "TCP" : "UDP",
                    k_Ports[i].port);
                message += msgLen;
                messageLength -= msgLen;

                // Keep going to check all ports and report the failing ones
                ret = false;
            }
        }
    }

    return ret;
}

bool FindLocalInterfaceIP4Address(PSOCKADDR_IN addr)
{
    SOCKET s;

    printf("Finding local IP address...");

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    SOCKADDR_IN sin = {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(443);
    sin.sin_addr.S_un.S_addr = inet_addr("8.8.8.8");
    int err = connect(s, (struct sockaddr*)&sin, sizeof(sin));
    if (err == SOCKET_ERROR) {
        printf("connect() failed: %d\n", WSAGetLastError());
        closesocket(s);
        return false;
    }

    // Determine which local interface we bound to
    int nameLen = sizeof(*addr);
    err = getsockname(s, (struct sockaddr*)addr, &nameLen);
    if (err == SOCKET_ERROR) {
        printf("getsockname() failed: %d\n", WSAGetLastError());
        closesocket(s);
        return false;
    }

    char addrStr[64];
    inet_ntop(AF_INET, &addr->sin_addr, addrStr, sizeof(addrStr));
    printf("%s\n", addrStr);

    return true;
}

enum UPnPPortStatus {
    NOT_FOUND,
    OK,
    CONFLICTED,
    ERRORED
};
UPnPPortStatus UPnPCheckPort(struct UPNPUrls* urls, struct IGDdatas* data, int proto, const char* myAddr, int port, char* conflictMessage)
{
    char intClient[16];
    char intPort[6];
    char desc[80];
    char enabled[4];
    char leaseDuration[16];
    const char* protoStr;
    char portStr[6];

    snprintf(portStr, sizeof(portStr), "%d", port);
    switch (proto)
    {
    case IPPROTO_TCP:
        protoStr = "TCP";
        break;
    case IPPROTO_UDP:
        protoStr = "UDP";
        break;
    default:
        assert(false);
        return ERRORED;
    }

    printf("Checking for UPnP port mapping for %s %s -> %s...", protoStr, portStr, myAddr);
    int err = UPNP_GetSpecificPortMappingEntry(
        urls->controlURL, data->first.servicetype, portStr, protoStr, nullptr,
        intClient, intPort, desc, enabled, leaseDuration);
    if (err == 714) {
        // NoSuchEntryInArray
        printf("NOT FOUND\n");
        return NOT_FOUND;
    }
    else if (err == UPNPCOMMAND_SUCCESS) {
        if (!strcmp(myAddr, intClient)) {
            printf("OK\n");
            return OK;
        }
        else {
            printf("CONFLICT - %s %s\n", desc, intClient);
            snprintf(conflictMessage, 128, "%s (%s)", desc, intClient);
            return CONFLICTED;
        }
    }
    else {
        printf("ERROR %d\n", err);
        return ERRORED;
    }
}

bool STUNFindWanAddress(PSOCKADDR_IN wanAddr)
{
    SOCKET s;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    struct hostent *host;

    host = gethostbyname("stun.stunprotocol.org");
    if (host == nullptr) {
        printf("gethostbyname() failed\n");
        closesocket(s);
        return false;
    }

    SOCKADDR_IN sin = {};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(3478);
    sin.sin_addr = *(struct in_addr*)host->h_addr;
    int err = connect(s, (struct sockaddr*)&sin, sizeof(sin));
    if (err == SOCKET_ERROR) {
        printf("connect() failed: %d\n", WSAGetLastError());
        closesocket(s);
        return false;
    }

    STUN_MESSAGE reqMsg;
    reqMsg.messageType = htons(STUN_MESSAGE_BINDING_REQUEST);
    reqMsg.messageLength = 0;
    reqMsg.magicCookie = htonl(STUN_MESSAGE_COOKIE);
    for (int i = 0; i < ARRAYSIZE(reqMsg.transactionId); i++) {
        rand_s(&reqMsg.transactionId[i]);
    }

    err = send(s, (char *)&reqMsg, sizeof(reqMsg), 0);
    if (err == SOCKET_ERROR) {
        printf("send() failed: %d\n", WSAGetLastError());
        closesocket(s);
        return false;
    }

    union {
        struct {
            STUN_MESSAGE respMsg;
            STUN_MAPPED_IPV4_ADDRESS_ATTRIBUTE mappedAddress;
        };
        char respBuf[128];
    };

    int bytesRead = recv(s, respBuf, sizeof(respBuf), 0);
    if (bytesRead == SOCKET_ERROR) {
        printf("recv() failed: %d\n", WSAGetLastError());
        closesocket(s);
        return false;
    }
    else if (bytesRead < sizeof(respMsg)) {
        printf("STUN message truncated: %d\n", bytesRead);
        closesocket(s);
        return false;
    }

    closesocket(s);

    if (htonl(respMsg.magicCookie) != STUN_MESSAGE_COOKIE) {
        printf("Bad STUN cookie value: %x\n", htonl(respMsg.magicCookie));
        return false;
    }
    else if (!RtlEqualMemory(reqMsg.transactionId, respMsg.transactionId, sizeof(reqMsg.transactionId))) {
        printf("STUN transaction ID mismatch\n");
        return false;
    }
    else if (htons(respMsg.messageType) != STUN_MESSAGE_BINDING_SUCCESS) {
        printf("STUN message type mismatch: %x\n", htons(respMsg.messageType));
        return false;
    }
    else if (bytesRead < sizeof(respMsg) + sizeof(mappedAddress)) {
        printf("STUN message too short: %d\n", bytesRead);
        return false;
    }
    else if (htons(mappedAddress.attributeType) != STUN_ATTRIBUTE_MAPPED_ADDRESS &&
        htons(mappedAddress.attributeType) != STUN_ATTRIBUTE_XOR_MAPPED_ADDRESS) {
        printf("STUN attribute type mismatch: %x\n", htons(mappedAddress.attributeType));
        return false;
    }
    else if (htons(mappedAddress.attributeLength) != 8) {
        printf("STUN address length mismatch: %d\n", htons(mappedAddress.attributeLength));
        return false;
    }
    else if (mappedAddress.addressFamily != 1) {
        printf("STUN address family mismatch: %x\n", mappedAddress.addressFamily);
        return false;
    }

    if (htons(mappedAddress.attributeType) == STUN_ATTRIBUTE_MAPPED_ADDRESS) {
        // The address is directly encoded
        wanAddr->sin_addr.S_un.S_addr = mappedAddress.address;
    }
    else {
        // The address is XORed
        wanAddr->sin_addr.S_un.S_addr = mappedAddress.address ^ respMsg.magicCookie;
    }

    return true;
}

bool CheckWANAccess(PSOCKADDR_IN wanAddr, bool* foundPortForwardingRules)
{
    natpmp_t natpmp;

    *foundPortForwardingRules = false;

    printf("Finding WAN IP address...");
    bool gotWanAddress = false;
    int natPmpErr = initnatpmp(&natpmp, 0, 0);
    if (natPmpErr != 0) {
        printf("initnatpmp() failed: %d\n", natPmpErr);
    }
    else {
        natPmpErr = sendpublicaddressrequest(&natpmp);
        if (natPmpErr < 0) {
            printf("sendpublicaddressrequest() failed: %d\n", natPmpErr);
            closenatpmp(&natpmp);
        }
    }

    {
        int upnpErr;
        struct UPNPDev* ipv4Devs = upnpDiscoverAll(5000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &upnpErr);

        struct UPNPUrls urls;
        struct IGDdatas data;
        char myAddr[128];
        char wanAddrStr[128];
        int ret = UPNP_GetValidIGD(ipv4Devs, &urls, &data, myAddr, sizeof(myAddr));
        if (ret != 0) {
            // Connected or disconnected IGD
            if (ret == 1 || ret == 2) {
                ret = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, wanAddrStr);
                if (ret == UPNPCOMMAND_SUCCESS) {
                    wanAddr->sin_addr.S_un.S_addr = inet_addr(wanAddrStr);
                    printf("%s (UPnP)\n", wanAddrStr);
                    gotWanAddress = true;
                }

                char conflictMessage[512];
                *foundPortForwardingRules = true;
                for (int i = 0; i < ARRAYSIZE(k_Ports); i++) {
                    char conflictEntry[128];
                    UPnPPortStatus status = UPnPCheckPort(&urls, &data, k_Ports[i].proto, myAddr, k_Ports[i].port, conflictEntry);
                    if (status != OK) {
                        *foundPortForwardingRules = false;
                    }
                    switch (status)
                    {
                    case CONFLICTED:
                        snprintf(conflictMessage, sizeof(conflictMessage),
                            "Detected a port forwarding conflict with another PC on your network: %s\n\n"
                            "Remove that PC from your network or uninstall the Moonlight Internet Streaming Service from it, then restart your router.",
                            conflictEntry);
                        DisplayMessage(conflictMessage);
                        return false;
                    default:
                        continue;
                    }
                }
            }

            FreeUPNPUrls(&urls);
        }
    }

    // Use the delay of upnpDiscoverAll() to also allow the NAT-PMP endpoint time to respond
    if (natPmpErr >= 0) {
        natpmpresp_t response;
        natPmpErr = readnatpmpresponseorretry(&natpmp, &response);
        closenatpmp(&natpmp);

        if (natPmpErr == 0 && !gotWanAddress) {
            char addrStr[64];
            wanAddr->sin_addr = response.pnu.publicaddress.addr;
            inet_ntop(AF_INET, &response.pnu.publicaddress.addr, addrStr, sizeof(addrStr));
            printf("%s (NAT-PMP)\n", addrStr);
            gotWanAddress = true;
        }
    }

    if (!gotWanAddress) {
        if (!STUNFindWanAddress(wanAddr)) {
            printf("FAILED\n");
            DisplayMessage("MIST was unable to determine your public IP address. Please check your Internet connection.");
            return false;
        }
        
        char addrStr[64];
        inet_ntop(AF_INET, &wanAddr->sin_addr, addrStr, sizeof(addrStr));
        printf("%s (STUN)\n", addrStr);
        return true;
    }
    else {
        return true;
    }
}

bool IsPossibleCGN(PSOCKADDR_IN wanAddr)
{
    DWORD addr = htonl(wanAddr->sin_addr.S_un.S_addr);

    // 10.0.0.0/8 - ISPs used to use this
    if ((addr & 0xFF000000) == 0x0A000000) {
        return true;
    }
    // 100.64.0.0/10 - RFC6598 official CGN address
    else if ((addr & 0xFFC0) == 0x64400000) {
        return true;
    }

    return false;
}

bool IsDoubleNAT(PSOCKADDR_IN wanAddr)
{
    DWORD addr = htonl(wanAddr->sin_addr.S_un.S_addr);

    // 10.0.0.0/8
    if ((addr & 0xFF000000) == 0x0A000000) {
        return true;
    }
    // 172.16.0.0/12
    else if ((addr & 0xFFF00000) == 0xAC100000) {
        return true;
    }
    // 192.168.0.0/16
    else if ((addr & 0xFFFF0000) == 0xC0A80000) {
        return true;
    }

    return false;
}

int main(int argc, char* argv[])
{
    WSADATA wsaData;

    char tempPath[MAX_PATH + 1];
    GetTempPathA(sizeof(tempPath), tempPath);

    snprintf(logFilePath, sizeof(logFilePath), "%s\\%s", tempPath, "mist.log");
    freopen(logFilePath, "w", stdout);

    int err = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (err != NO_ERROR) {
        DisplayMessage("Unable to initialize WinSock");
        return err;
    }

    fprintf(stderr, "Checking if GameStream is enabled...\n");

    // First check if GameStream is enabled
    if (!IsGameStreamEnabled()) {
        return -1;
    }

    union {
        SOCKADDR_STORAGE ss;
        SOCKADDR_IN sin;
        SOCKADDR_IN6 sin6;
    };
    char msgBuf[2048];

    fprintf(stderr, "Testing local GameStream connectivity...\n");

    // Try to connect via IPv4 loopback
    ss = {};
    sin.sin_family = AF_INET;
    sin.sin_addr = in4addr_loopback;
    printf("Testing GameStream ports via loopback\n");
    if (!TestAllPorts(&ss,
        "Local GameStream connectivity check failed. Please try reinstalling GeForce Experience.\n\nThe following ports were not working:\n",
        msgBuf, sizeof(msgBuf))) {
        DisplayMessage(msgBuf);
        return -1;
    }

    if (!FindLocalInterfaceIP4Address(&sin)) {
        DisplayMessage("Unable to perform GameStream connectivity check. Please check your Internet connection and try again.");
        return -1;
    }

    fprintf(stderr, "Testing network GameStream connectivity...\n");

    // Try to connect via LAN IPv4 address
    printf("Testing GameStream ports via local network\n");
    if (!TestAllPorts(&ss,
        "Local network GameStream connectivity check failed. Try temporarily disabling your firewall software or adding firewall exceptions for the following ports:\n",
        msgBuf, sizeof(msgBuf))) {
        DisplayMessage(msgBuf);
        return -1;
    }

    fprintf(stderr, "Detecting public IP address...\n");

    bool upnpRulesFound;
    if (!CheckWANAccess(&sin, &upnpRulesFound)) {
        return -1;
    }

    fprintf(stderr, "Testing Internet GameStream connectivity...\n");

    // Try to connect via WAN IPv4 address
    printf("Testing GameStream ports via WAN address\n");
    if (!TestAllPorts(&ss,
        upnpRulesFound ? "Found UPnP rules, but they did not work correctly. Check for conflicting port forwarding entries in your router settings.\n\nThe following ports were not forwarded properly:\n" :
        "Internet GameStream connectivity check failed. Make sure UPnP is enabled in your router settings.\n\nThe following ports were not forwarded properly:\n",
        msgBuf, sizeof(msgBuf))) {
        DisplayMessage(msgBuf);
        return -1;
    }

    // Check for double-NAT
    if (IsDoubleNAT(&sin)) {
        DisplayMessage("Your router appears be connected to another router. This configuration breaks port forwarding. To resolve this, switch one of the devices into bridge mode.");
        return -1;
    }
    // Check for CGN
    else if (IsPossibleCGN(&sin)) {
        DisplayMessage("Your ISP is running a Carrier-Grade NAT. This prevents you from hosting services like GameStream. Contact your ISP to get a real public IP address.");
        return -1;
    }

    char addrStr[64];
    inet_ntop(AF_INET, &sin.sin_addr, addrStr, sizeof(addrStr));
    snprintf(msgBuf, sizeof(msgBuf), "All tests passed! You should be able to stream by typing the following address into Moonlight's Add PC dialog: %s", addrStr);
    DisplayMessage(msgBuf, false);

    return 0;
}