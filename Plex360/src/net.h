// net.h — client HTTP minimal sur sockets BSD (winsockx) pour Xbox 360
#pragma once
#include <xtl.h>

namespace Net {

// XNetStartup + WSAStartup + attente adresse. A appeler une fois au boot.
// Retourne 0 = OK. Sinon code d'etape : 1=XNetStartup, 2=pas d'IP, 3=WSAStartup.
DWORD Init();

// "192.168.1.42" une fois Init OK ("" sinon). Utile pour debug subnet.
const char* LocalIPStr();

// GET bloquant. extraHeaders peut etre NULL ou contenir des lignes "K: V\r\n".
// Succes : outData (a free() par l'appelant), outSize, status HTTP.
// Echec : retourne false, outData=NULL. LastStage/LastErr donnent le detail :
//   1=resolve 2=socket 3=connect 4=send 5=recv-vide 6=pas-de-headers 7=proto
bool HttpGet(const char* host, WORD port, const char* path,
             const char* extraHeaders,
             BYTE** outData, DWORD* outSize, DWORD* outStatus);
int LastStage();   // etape ou le dernier HttpGet a echoue (0 = OK)
int LastErr();     // WSAGetLastError au moment de l'echec

// Timeout send/recv des appels suivants (defaut 10s). Le relay transcodeur
// peut mettre du temps a produire les donnees -> appeler avec ~60s avant
// une lecture stream, puis remettre 10s.
void SetIoTimeout(int ms);

// Sonde TCP non bloquante : retourne vrai si connect() reussit.
// timeoutMs court (~3s). Err detaille dans LastErr() (10060 timeout,
// 10061 refuse, 0 = OK).
bool Probe(const char* ip, WORD port, int timeoutMs);

void Shutdown();

}
