
/* 
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
  
Original software by KI4LKF



Version 3.08a, modded by Bob Scott, W6KD  
July 4th, 2013

  Adds password protection for command processor
  Usage: ul passwd - unlocks command line processor for remote inputs via netcat 
                     (cmd processor defaults to locked and auto locks 60 sec after last valid command)
         lk        - locks command line processor

  Default Password: xrfcmd
  Password is set in /root/dxrfd/passwd.h BEFORE compiling XRF with dxrfd.doit script

Command port changed from 30002 to 30010 to accomodate changes in ircDDBGateway port usage

Version 3.09, modded by Russell Thomas, KV4S
January 23rd, 2020
	Attempt to unlock all letter modules.

*/


#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include "GitVersion.h"

#include <regex.h>
static regex_t preg;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

/* Required for Binary search trees using C++ STL */
#include <string>
#include <map>
#include <set>
#include <utility>
using namespace std;

#include <pthread.h>

#include </opt/dxrfd/passwd.h>  /* Set command line password in passwd.h */

#define VERSION "3.09-kv4s"
#define OWNER_SIZE 8
#define IP_SIZE 15
#define MAXHOSTNAMELEN 64
#define CALL_SIZE 8

/* 
   Timeout is 30 seconds.
   If after 30 seconds, we have not received the KEEPALIVE,
   we drop that station
*/
static short TIMEOUT = 60;

static short pwunlock = 0;
static time_t unlocktime;

static char STATUS_FILE[FILENAME_MAX + 1];

/* configuration data */
/* Put that in a structure, later */
static char OWNER[OWNER_SIZE + 1];
static char ADMIN[CALL_SIZE + 1];
static char LISTEN_IP[IP_SIZE + 1];
static int LISTEN_PORT = 30001;
static int COMMAND_PORT = 30010;

/* max number of XRF repeaters */
static unsigned int MAX_USERS = 10;

/* max number of dvap/dvtool dongles */
static unsigned int MAX_OTHER_USERS=10;

static bool QSO_DETAILS = false;
static char USERS[FILENAME_MAX + 1];
static char BLOCKS[FILENAME_MAX + 1];

// Input from XRF repeaters
static int srv_sock = -1;

// Input from admin commands
static int cmd_sock = -1;

// Input from dvap, dvtool dongles
static int ref_sock = -1;

/* ACK back to dvap, dvtool users */
static unsigned char REF_ACK[3] = { 3, 96, 0 };

/*** for replying with DASHBOARD information ***/
#define LH_MAX_SIZE 39
typedef map<string, string> dt_lh_type;
static dt_lh_type dt_lh_list;

/* inbound dongles(dvap, dvtool, ...) on port 20001 */
struct inbound
{
   /* the callsign of the remote */
   char call[CALL_SIZE + 1];

   /* if true, packets from this call are dropped */
   bool isMute;

   time_t connect_time;

   /* IP and port of remote */
   struct sockaddr_in sin;

   /* if countdown expires, the connection is terminated */
   short countdown;

   /* This user talked on this module */
   char mod;  /* A B C D E*/

   bool is_ref; 
   char links[26]; 
   /* 
      The index is the local module: 0=A, 1=B, 2=C, 3=D, 4=E
      The value is the remote module
      Example:  links[1] = 'C'  
      That means that our local module B is linked to remote module C
      The remote system is identified by inbound->call
   */ 

   /* the serial number of the dongle */
   char serial[9];

};
/* the Key in this inbound_list map is the unique IP-port address of the remote, example:  x.x.x.x-20001 */
typedef map<string, inbound *> inbound_type;
static inbound_type inbound_list;

/* 
   Just before we send data to dvtool/dvap users, 
   we save the header here, for re-transmit later
*/
static struct
{
   uint32_t s_addr;
   unsigned char hdr[58];
} temp_r[26];

/* inbound repeaters on port 30001 */
struct a_user
{
   /* callsign of the connected repeater */
   char call[CALL_SIZE + 1];

   /* if true, packets from this call are dropped */
   bool isMute;

   /* is this another XRF reflector */
   bool is_xrf; 

   time_t connect_time;

   /* The first index identifies the local reflector module
      index 0 identifies reflector module A
      index 1 identifies reflector module B
      index 2 identifies reflector module C
      index 3 identifies reflector module D
      index 4 identifies reflector module E

      The second index identifies the remote repeater band.
      from 0 to 3
      which is from A...D

      Example:
               rpt_mods[1][2] = 'C'
               rpt_mods[1][3] = 'D'

      Explanation:
               Reflector module B is linked to repeater module C
               Reflector module B is linked to repeater module D
          This means that the remote repeater 
               has linked both repeater bands C and D to our reflector module B
   */
   char rpt_mods[26][4];

   /* time link was established */
   time_t link_time[26][4];

   /* IP address and UDP port of connected station */
   /* For easy access to the connected station */
   struct sockaddr_in sin;

   /* if countdown expires, the connection is terminated */
   short countdown;

   /* This user talked on this module */
   char mod; /* A B C D E */

};
/* The BST/map of connected users */
/* The KEY is the unique IP address string of ip[IP_SIZE + 1] */
/* The data is a pointer to struct a_user */
typedef map<string, struct a_user *> a_user_list_type;
static a_user_list_type a_user_list;

/* 
   Just before we send the data to XRF repeaters
   we save the header here for re-transmit later
*/
static struct
{
   uint32_t s_addr;
   unsigned char hdr[56];
   unsigned char old_sid[2];
} temp_x[26];

/* the map of which reflectors we can link to */
typedef map<string,string> call_ip_type;
static call_ip_type call_ip_map;

/* 
   The BST/set of blocked callsigns.
   The KEY is the unique blocked callsign.
   There is no data in the set.
   Blocked callsigns added by the administrator of the reflector. 
*/
typedef set<string> blocks_type;
static blocks_type blocks; 

/* socket descriptor set */
static fd_set fdset;

/* 20 ms delay */
static struct timeval tv;

static time_t tNow = 0;
/* timing for XRF users */
static time_t HBinterStart = 0;
/* timing for dvtool/dvap users */
static time_t inboundStart = 0;

/* Variables used by more than one function */
static const short READBUFFER_SIZE = 1024;
static unsigned char readBuffer[READBUFFER_SIZE];
static struct sockaddr_in fromUser;
static struct sockaddr_in fromCmd;

/* dvap, dvtool input buffer */
static unsigned char refbuf[READBUFFER_SIZE];
static struct sockaddr_in fromInbound;

/* status file */
static FILE *statusfp = NULL;

/*** rcd data ***/
static pthread_t playback_thread;
pthread_attr_t attr;
/* 30 users recording at the same time */
static const unsigned short int MAX_RCD_USERS = 30;
/* each user can record 750 packets = 15 seconds of recorded audio */
static const unsigned short int MAX_RCD_DATA = 750;
struct rcd
{
   bool locked;
   time_t ts;
   struct sockaddr_in sin;
   short int recvlen;
   unsigned short idx; /*** index into data ***/
   unsigned char data[MAX_RCD_DATA][58]; /*** 10 seconds ***/
};
/* key is streamid */
typedef map<string, struct rcd *> rcd_list_type;
static rcd_list_type rcd_list;
static struct rcd *an_rcd;
static rcd_list_type::iterator rcd_pos;
static pair<rcd_list_type::iterator,bool> rcd_insert_pair;
static char an_rcd_streamid[32];
static time_t check_rcd_time = 0;

static bool keep_running = true;
static u_int16_t streamid_raw = 0;

/* The reflector uses these functions only */
static void check_heartbeat();
static void print_users();
static void print_version();
static void mute_users(bool mute);
static bool mute_call(char *call, bool mute);
static void print_blocks();
static void print_links_file();
static void print_links_screen();
static bool get_ip(char *call, char *ip);
static void handle_cmd(char *buf);
static void runit();
static int  read_config(char *);
static int  srv_open();
static int  cmd_open();
static void sigCatch(int signum);
static int open_users(char *filename);
static int open_blocks(char *filename);
static bool resolve_rmt(char *name, int type, struct sockaddr_in *addr);
static void *playback(void *arg);

/* dvap, dongles, ... */
static void send_heartbeat();
static int  ref_open();
static int link_to_ref(char *call);
static int link_to_xrf(char local_mod, char *ref, char remote_mod, char *IP);

static bool resolve_rmt(char *name, int type, struct sockaddr_in *addr)
{
   struct addrinfo hints;
   struct addrinfo *res;
   struct addrinfo *rp;
   int rc = 0;
   bool found = false;

   memset(&hints, 0x00, sizeof(struct addrinfo));
   hints.ai_family = AF_INET;
   hints.ai_socktype = type;

   rc = getaddrinfo(name, NULL, &hints, &res);
   if (rc != 0)
   {
      printf("getaddrinfo return error code %d for [%s]\n", rc, name);
      return false;
   }

   for (rp = res; rp != NULL; rp = rp->ai_next)
   {
      if ((rp->ai_family == AF_INET) &&
          (rp->ai_socktype == type))
      {
         memcpy(addr, rp->ai_addr, sizeof(struct sockaddr_in));
         found = true;
         break;
      }
   }
   freeaddrinfo(res);
   return found;
}

static int link_to_xrf(char local_mod, char *ref, char remote_mod, char *IP)
{
   short counter;
   char request[CALL_SIZE + 3];
   struct sockaddr_in sin;
   a_user_list_type::iterator user_pos;

   if (remote_mod == 'X')
      remote_mod = ' ';  /* unlink request */
   else
   {
      user_pos = a_user_list.find(IP);
      if (user_pos != a_user_list.end())
      {
         /* It already exists */
         printf("Remote reflector [%s] ip=%s already linked, unlink first\n", ref, IP); 
         return 0;
      }
   }

   /* send the request to the remote XRF reflector */
   strcpy(request, OWNER);
   request[8] = local_mod;
   request[9] = remote_mod;
   request[10] = '\0';

   memset(&sin,0,sizeof(struct sockaddr_in));
   sin.sin_family = AF_INET;
   sin.sin_port = htons(LISTEN_PORT);
   sin.sin_addr.s_addr = inet_addr(IP);

   printf("Sending request [%s] to reflector %s\n", request, ref);

   for (counter = 0; counter < 26; counter++)
      sendto(srv_sock, request, CALL_SIZE + 3,
             0,(struct sockaddr *)&sin,
             sizeof(struct sockaddr_in));

   return 0;
}

static int link_to_ref(char *call)
{
   char payload[MAXHOSTNAMELEN + 1];

   /* IP-port */
   char search_value[MAXHOSTNAMELEN + 7];

   inbound_type::iterator inbound_pos;
   inbound *inbound_ptr;
   pair<inbound_type::iterator,bool> inbound_insert_pair;
   struct sockaddr_in sin;
   unsigned char queryCommand[56];
   unsigned short j;

   char local_mod = ' ';
   char ref[CALL_SIZE + 1];
   char remote_mod = ' ';

   local_mod = call[0];

	/*
   if ((local_mod != 'A') && (local_mod != 'B') && 
       (local_mod != 'C') && (local_mod != 'D')) 
   {
      printf("Invalid local module %c for linking\n", local_mod);
      return -1;
   }
   */

   memset(ref, ' ', sizeof(ref));
   memcpy(ref, call + 1, 6);
   ref[8] = '\0'; 

   /* Is it blocked ? */
   if (blocks.find(ref) != blocks.end())
      return -1;

   if ((memcmp(ref, "REF", 3) != 0) && (memcmp(ref, "XRF", 3) != 0))
   {
      printf("XRF or REF only\n");
      return -1;
   }

   if (strcmp(ref, OWNER) == 0)
   {
      printf("Can not link to itself\n");
      return -1;
   }

   remote_mod = call[7];
   /*
   if ((remote_mod != 'A') && (remote_mod != 'B') && (remote_mod != 'C') && 
       (remote_mod != 'D') && (remote_mod != 'X'))
   {
      printf("Invalid remote module %c\n", remote_mod);
      return -1;
   }
   */

   payload[0] = '\0';
   /* get the IP address from database */
   if (!get_ip(ref, payload))
      return -1;

   /* No IP in db? */
   if (payload[0] == '\0')
   {
      printf("No host or IP address for %s\n", ref);
      return -1;
   }

   if (strcmp(payload, "0.0.0.0") == 0)
   {
      printf("Invalid IP in db\n");
      return -1;
   }

   if (memcmp(ref, "XRF", 3) == 0)
   {
      return link_to_xrf(local_mod, ref, remote_mod, payload);
   }

   /* Is it already linked? */
   sprintf(search_value, "%s-20001", payload);

   inbound_pos = inbound_list.find(search_value);
   if (inbound_pos != inbound_list.end())
   {
      inbound_ptr = (inbound *)inbound_pos->second;
      if (!inbound_ptr->is_ref)
      {
         printf("This connection is not a reflector\n");
         return -1;
      }

      /* update the links */
      if (remote_mod == 'X')
         remote_mod = ' ';
      else
      if ((inbound_ptr->links[0] == remote_mod) ||
          (inbound_ptr->links[1] == remote_mod) ||
          (inbound_ptr->links[2] == remote_mod) ||
          (inbound_ptr->links[3] == remote_mod) ||
          (inbound_ptr->links[4] == remote_mod) ||
		  (inbound_ptr->links[5] == remote_mod) ||
		  (inbound_ptr->links[6] == remote_mod) ||
		  (inbound_ptr->links[7] == remote_mod) ||
		  (inbound_ptr->links[8] == remote_mod) ||
		  (inbound_ptr->links[9] == remote_mod) ||
		  (inbound_ptr->links[10] == remote_mod) ||
		  (inbound_ptr->links[11] == remote_mod) ||
		  (inbound_ptr->links[12] == remote_mod) ||
		  (inbound_ptr->links[13] == remote_mod) ||
		  (inbound_ptr->links[14] == remote_mod) ||
		  (inbound_ptr->links[15] == remote_mod) ||
		  (inbound_ptr->links[16] == remote_mod) ||
		  (inbound_ptr->links[17] == remote_mod) ||
		  (inbound_ptr->links[18] == remote_mod) ||
		  (inbound_ptr->links[19] == remote_mod) ||
		  (inbound_ptr->links[20] == remote_mod) ||
		  (inbound_ptr->links[21] == remote_mod) ||
		  (inbound_ptr->links[22] == remote_mod) ||
		  (inbound_ptr->links[23] == remote_mod) ||
		  (inbound_ptr->links[24] == remote_mod) ||
		  (inbound_ptr->links[25] == remote_mod))
      {
         printf ("Already set or duplicate assignment\n");
         return 0;
      }

      if (local_mod == 'A')
         inbound_ptr->links[0] = remote_mod;
      else
      if (local_mod == 'B')
         inbound_ptr->links[1] = remote_mod;
      else
      if (local_mod == 'C')
         inbound_ptr->links[2] = remote_mod;
      else
      if (local_mod == 'D')
         inbound_ptr->links[3] = remote_mod;
	 else
      if (local_mod == 'E')
         inbound_ptr->links[4] = remote_mod;
	 else
      if (local_mod == 'F')
         inbound_ptr->links[5] = remote_mod;
	 else
      if (local_mod == 'G')
         inbound_ptr->links[6] = remote_mod;
	 else
      if (local_mod == 'H')
         inbound_ptr->links[7] = remote_mod;
	 else
      if (local_mod == 'I')
         inbound_ptr->links[8] = remote_mod;
	 else
      if (local_mod == 'J')
         inbound_ptr->links[9] = remote_mod;
	 else
      if (local_mod == 'K')
         inbound_ptr->links[10] = remote_mod;
	 else
      if (local_mod == 'L')
         inbound_ptr->links[11] = remote_mod;
	 else
      if (local_mod == 'M')
         inbound_ptr->links[12] = remote_mod;
	 else
      if (local_mod == 'N')
         inbound_ptr->links[13] = remote_mod;
	 else
      if (local_mod == 'O')
         inbound_ptr->links[14] = remote_mod;
	 else
      if (local_mod == 'P')
         inbound_ptr->links[15] = remote_mod;
	 else
      if (local_mod == 'Q')
         inbound_ptr->links[16] = remote_mod;
	 else
      if (local_mod == 'R')
         inbound_ptr->links[17] = remote_mod;
	 else
      if (local_mod == 'S')
         inbound_ptr->links[18] = remote_mod;
	 else
      if (local_mod == 'T')
         inbound_ptr->links[19] = remote_mod;
	 else
      if (local_mod == 'U')
         inbound_ptr->links[20] = remote_mod;
	 else
      if (local_mod == 'V')
         inbound_ptr->links[21] = remote_mod;
	 else
      if (local_mod == 'W')
         inbound_ptr->links[22] = remote_mod;
	 else
      if (local_mod == 'X')
         inbound_ptr->links[23] = remote_mod;
	 else
      if (local_mod == 'Y')
         inbound_ptr->links[24] = remote_mod;
      else
         inbound_ptr->links[25] = remote_mod;

      if ((inbound_ptr->links[0] == ' ') && 
          (inbound_ptr->links[1] == ' ') &&
          (inbound_ptr->links[2] == ' ') &&
          (inbound_ptr->links[3] == ' ') &&
		  (inbound_ptr->links[4] == ' ') &&
		  (inbound_ptr->links[5] == ' ') &&
		  (inbound_ptr->links[6] == ' ') &&
		  (inbound_ptr->links[7] == ' ') &&
		  (inbound_ptr->links[8] == ' ') &&
		  (inbound_ptr->links[9] == ' ') &&
		  (inbound_ptr->links[10] == ' ') &&
		  (inbound_ptr->links[11] == ' ') &&
		  (inbound_ptr->links[12] == ' ') &&
		  (inbound_ptr->links[13] == ' ') &&
		  (inbound_ptr->links[14] == ' ') &&
		  (inbound_ptr->links[15] == ' ') &&
		  (inbound_ptr->links[16] == ' ') &&
		  (inbound_ptr->links[17] == ' ') &&
		  (inbound_ptr->links[18] == ' ') &&
		  (inbound_ptr->links[19] == ' ') &&
		  (inbound_ptr->links[20] == ' ') &&
		  (inbound_ptr->links[21] == ' ') &&
		  (inbound_ptr->links[22] == ' ') &&
		  (inbound_ptr->links[23] == ' ') &&
		  (inbound_ptr->links[24] == ' ') &&
          (inbound_ptr->links[25] == ' '))
          
      {
         /* all links removed, disconnect from remote system */

         printf("All links removed, disconnecting from %s\n", inbound_ptr->call);
         refbuf[0] = 5;
         refbuf[1] = 0;
         refbuf[2] = 24;
         refbuf[3] = 0;
         refbuf[4] = 0;
         sendto(ref_sock,(char *)refbuf,5,0,
                (struct sockaddr *)&(inbound_ptr->sin),
                sizeof(struct sockaddr_in));

         free(inbound_pos->second);
         inbound_pos->second = NULL;
         inbound_list.erase(inbound_pos);
      } 
      print_links_file();
      return 0;
   }

   /*
   if (remote_mod == 'X')
   {
      printf("X can not be used, there is no connection to unlink\n");
      return -1;
   }
   */

   if ((inbound_list.size() + 1) > MAX_OTHER_USERS)
   {
      printf("Over the MAX_OTHER_USERS limit of %d\n", MAX_OTHER_USERS);
      return -1;
   }

   sin.sin_family = AF_INET;
   sin.sin_addr.s_addr = inet_addr(payload);
   sin.sin_port = htons(20001);
  
   /* assume that we will be ok */
   inbound_ptr = (inbound *)malloc(sizeof(inbound));
   if (inbound_ptr)
   {
      inbound_ptr->countdown = TIMEOUT;
      memcpy((char *)&(inbound_ptr->sin),(char *)&sin, sizeof(struct sockaddr_in));
      strcpy(inbound_ptr->call, ref);
      time(&(inbound_ptr->connect_time));
      inbound_ptr->isMute = false;
      inbound_ptr->mod = ' ';
      inbound_ptr->is_ref = true;
      inbound_ptr->links[0] = inbound_ptr->links[1] = inbound_ptr->links[2] = inbound_ptr->links[3] = inbound_ptr->links[4] = ' '; 
      if (local_mod == 'A')
         inbound_ptr->links[0] = remote_mod;
      else
      if (local_mod == 'B')
         inbound_ptr->links[1] = remote_mod;
      else
      if (local_mod == 'C')
         inbound_ptr->links[2] = remote_mod;
      else
      if (local_mod == 'D')
         inbound_ptr->links[3] = remote_mod;
	 else
      if (local_mod == 'E')
         inbound_ptr->links[4] = remote_mod;
	 else
      if (local_mod == 'F')
         inbound_ptr->links[5] = remote_mod;
	 else
      if (local_mod == 'G')
         inbound_ptr->links[6] = remote_mod;
	 else
      if (local_mod == 'H')
         inbound_ptr->links[7] = remote_mod;
	 else
      if (local_mod == 'I')
         inbound_ptr->links[8] = remote_mod;
	 else
      if (local_mod == 'J')
         inbound_ptr->links[9] = remote_mod;
	 else
      if (local_mod == 'K')
         inbound_ptr->links[10] = remote_mod;
	 else
      if (local_mod == 'L')
         inbound_ptr->links[11] = remote_mod;
	 else
      if (local_mod == 'M')
         inbound_ptr->links[12] = remote_mod;
	 else
      if (local_mod == 'N')
         inbound_ptr->links[13] = remote_mod;
	 else
      if (local_mod == 'O')
         inbound_ptr->links[14] = remote_mod;
	 else
      if (local_mod == 'P')
         inbound_ptr->links[15] = remote_mod;
	 else
      if (local_mod == 'Q')
         inbound_ptr->links[16] = remote_mod;
	 else
      if (local_mod == 'R')
         inbound_ptr->links[17] = remote_mod;
	 else
      if (local_mod == 'S')
         inbound_ptr->links[18] = remote_mod;
	 else
      if (local_mod == 'T')
         inbound_ptr->links[19] = remote_mod;
	 else
      if (local_mod == 'U')
         inbound_ptr->links[20] = remote_mod;
	 else
      if (local_mod == 'V')
         inbound_ptr->links[21] = remote_mod;
	 else
      if (local_mod == 'W')
         inbound_ptr->links[22] = remote_mod;
	 else
      if (local_mod == 'X')
         inbound_ptr->links[23] = remote_mod;
	 else
      if (local_mod == 'Y')
         inbound_ptr->links[24] = remote_mod;
      else
         inbound_ptr->links[25] = remote_mod;

      strcpy(inbound_ptr->serial, "REF     ");

      inbound_insert_pair = inbound_list.insert(pair<string, inbound *>(search_value, inbound_ptr));
      if (inbound_insert_pair.second)
      {
         printf("new CALL=%s,REPEATER,ip=%s, users=%ld\n",
                  inbound_ptr->call,search_value,inbound_list.size() + a_user_list.size());

         /* request to connect */
         queryCommand[0] = 5;
         queryCommand[1] = 0;
         queryCommand[2] = 24;
         queryCommand[3] = 0;
         queryCommand[4] = 1;
         for (j = 0; j < 2; j++)
            sendto(ref_sock,(char *)queryCommand,5,0,
                   (struct sockaddr *)&(inbound_ptr->sin),
                   sizeof(struct sockaddr_in));

         print_links_file();
      }
      else
      {
         printf("failed to add CALL=%s,ip=%s\n",inbound_ptr->call,search_value);
         free(inbound_ptr);
         inbound_ptr = NULL;
         return -1;
      }
   }
   else
   {
      printf("malloc() failed for call=%s,ip=%s\n",ref,search_value);
      return -1;
   }
   return 0;
}

/* send keepalive to dvap, dongles, ... */
static void send_heartbeat()
{
   inbound_type::iterator pos;
   inbound_type::iterator temp_pos = inbound_list.end();
   inbound *inbound_ptr;
   blocks_type::iterator block_pos;
   char dropped = ' ';
  
   for (pos = inbound_list.begin(); pos != inbound_list.end(); pos++)
   {
      inbound_ptr = (inbound *)pos->second;
 
      block_pos = blocks.find(inbound_ptr->call);
 
      if (block_pos == blocks.end())  /* not blocked */
      { 
         sendto(ref_sock,(char *)REF_ACK,3,0,
                (struct sockaddr *)&(inbound_ptr->sin),
                sizeof(struct sockaddr_in));
   
         if (inbound_ptr->countdown >= 0)
            inbound_ptr->countdown --;

         if (inbound_ptr->countdown < 0)
         {
            dropped = 't'; // timeout
            temp_pos = pos;
         }
      }
      else
      {
         dropped = 'b'; // blocked
         temp_pos = pos;
      }

   }

   if (temp_pos != inbound_list.end())
   {
      inbound_ptr = (inbound *)temp_pos->second;

      printf("call=%s %s, removing %s\n", 
                inbound_ptr->call, 
                (dropped == 't')?"timeout":"blocked",
                temp_pos->first.c_str());

      if (dropped == 'b')
      {
         /* disconnect */
         refbuf[0] = 5;
         refbuf[1] = 0;
         refbuf[2] = 24;
         refbuf[3] = 0;
         refbuf[4] = 0;
         sendto(ref_sock,(char *)refbuf,5,0,
                (struct sockaddr *)&(inbound_ptr->sin),
                sizeof(struct sockaddr_in));
      }

      free(temp_pos->second);
      temp_pos->second = NULL;
      inbound_list.erase(temp_pos);

      print_links_file();
   }
}

static int open_blocks(char *filename)
{
   FILE *fp = NULL;
   char inbuf[512];
   char *p = NULL;
   blocks_type::iterator pos;
   short int i;

   char *call_ptr;
   char call[9];
   const char *delim = " \t";

   fp = fopen(filename, "r");
   if (!fp)
   {
      printf("Failed to open file %s\n", filename);
      return 1;
   }

   blocks.clear();

   printf("Reading from file: [%s]\n", filename);
   while (fgets(inbuf, 511, fp) != NULL)
   {
      inbuf[511] = '\0';
      p = strchr(inbuf, '\r');
      if (p)
         *p = '\0';
      p = strchr(inbuf, '\n');
      if (p)
         *p = '\0';

      call_ptr = strtok(inbuf, delim);
      if (call_ptr)
      {
         if (strlen(call_ptr) <= 8)
         {
            memset(call, ' ', 9); call[8] = '\0';
            memcpy(call, call_ptr, strlen(call_ptr));

            for (i = 0; i < CALL_SIZE; i++)
            {
               if (call[i] == '_')
                  call[i] = ' ';
            }

            blocks.insert(call);
         }
      }
   }
   fclose(fp);

   printf("BEGIN listing blocked callsigns...%ld calls blocked\n", blocks.size());
   for (pos = blocks.begin(); pos != blocks.end(); pos++)
      printf("--->[%s]\n", pos->c_str());
   printf("END listing blocked callsigns\n");

   return 0;
}

static int open_users(char *filename)
{
   FILE *fp = NULL;
   char inbuf[512];
   char *p = NULL;
   
   char *gwy_ptr;
   char gwy[9];
   char *host_ptr;
   const char *delim = " \t";
   call_ip_type::iterator pos;

   fp = fopen(filename, "r");
   if (!fp)
   {
      printf("Failed to open file %s\n", filename);
      return 1;
   }

   call_ip_map.clear();

   printf("Reading from file: [%s]\n", filename);
   while (fgets(inbuf, 511, fp) != NULL)
   {
      inbuf[511] = '\0';
      p = strchr(inbuf, '\r');
      if (p)
         *p = '\0';
      p = strchr(inbuf, '\n');
      if (p)
         *p = '\0';

      gwy_ptr = strtok(inbuf, delim);
      host_ptr = strtok(NULL, delim);

      if (gwy_ptr && host_ptr)
      {
         if (strlen(gwy_ptr) <= 8)
         {
            memset(gwy, ' ', 9); gwy[8] = '\0';
            memcpy(gwy, gwy_ptr, strlen(gwy_ptr));
            call_ip_map[gwy] = host_ptr;
         }
      }
   }
   fclose(fp);

   for (pos = call_ip_map.begin(); pos != call_ip_map.end(); pos++)
      printf("[%s],[%s]\n", pos->first.c_str(), pos->second.c_str());
   printf("Loaded %ld entries\n", call_ip_map.size());

   return 0;
}

/* Search for that host */
static bool get_ip(char *call, char *ip)
{
   bool ok = false;
   call_ip_type::iterator pos;

   struct sockaddr_in a_net_addr;

   pos = call_ip_map.find(call);
   if (pos == call_ip_map.end())
   {
      printf("Callsign [%s] not found in database\n", call);
      return false;  // not found
   }

   ok = resolve_rmt((char *)pos->second.c_str(), SOCK_DGRAM, &(a_net_addr));
   if (!ok)
   {
      printf("Failed to resolve [%s] for callsign [%s]\n", 
               pos->second.c_str(), call); 
      return false;
   }

   if (ip)
      strcpy(ip, inet_ntoa(a_net_addr.sin_addr));

   return true;
}

/* Process the configuration file */
static int read_config(char *cfgFile)
{
   short int valid_params = 11;
   short int params = 0;

   FILE *cnf = NULL;
   char inbuf[1024];
   char *p;
   char *ptr = NULL;
   unsigned short int i;
   unsigned short int j;

   cnf = fopen(cfgFile, "r");
   if (!cnf)
   {
      printf("Failed to open file %s\n", cfgFile);
      return 1;
   }
   printf("Starting dxrfd v3.08a-w1bsb (GitID #%.7s)\n", gitversion);
   printf("dxrfd comes with ABSOLUTELY NO WARRANTY; see the LICENSE for details.\n");
   printf("This is free software, and you are welcome to distribute it\n");
   printf("under certain conditions that are discussed in the LICENSE file.\n");
 
   printf("Reading file %s\n", cfgFile);
   while (fgets(inbuf, 1020, cnf) != NULL)
   {
      if (strchr(inbuf, '#'))
         continue;

      p = strchr(inbuf, '\r');
      if (p)
         *p = '\0';
      p = strchr(inbuf, '\n');
      if (p)
         *p = '\0';

      p = strchr(inbuf, '=');
      if (!p)
         continue;
      *p = '\0';

      if (strcmp(inbuf,"OWNER") == 0)
      {
          memset(OWNER,' ', sizeof(OWNER));
          OWNER[OWNER_SIZE] = '\0';

          ptr = strchr(p + 1, ' ');
          if (ptr)
             *ptr = '\0';
          
          if (strlen(p + 1) != OWNER_SIZE - 2)
             printf("OWNER value %s invalid, length must be exactly %d\n", 
                      p + 1, OWNER_SIZE - 2);
          else
          {
             memcpy(OWNER, p + 1, OWNER_SIZE - 2);

             for (i = 0; i < strlen(OWNER); i++)
                OWNER[i] = toupper(OWNER[i]);

             if ((memcmp(OWNER, "XRF", 3) != 0)) 
                printf("Reflector names must be XRFzzz where zzz are digits from 1 thru 9, and A-Z.\n");
             else
             {
                printf("OWNER=%s\n",OWNER);
                params ++;
             }
          }
      }
      else
      if (strcmp(inbuf,"ADMIN") == 0)
      {
          memset(ADMIN,' ', sizeof(ADMIN));
          ADMIN[CALL_SIZE] = '\0';

          /* no spaces after the equal sign */
          if (p[1] == ' ')
             printf("ADMIN: no spaces after the equal sign\n");
          else
          {
             /* take up to 8 characters, throw away the rest */
             p[CALL_SIZE + 1] = '\0';

             /* valid length? */
             if ((strlen(p + 1) < 3) || (strlen(p + 1) > CALL_SIZE))
                printf("ADMIN value [%s] invalid\n", p + 1);
             else
             {
                memcpy(ADMIN, p + 1, strlen(p + 1));

                /* uppercase it */
                for (j = 0; j < CALL_SIZE; j++)
                   ADMIN[j] = toupper(ADMIN[j]);

                printf("ADMIN=[%s]\n",ADMIN);
                params ++;
             }
          }
      }
      else
      if (strcmp(inbuf,"LISTEN_IP") == 0)
      {
         ptr = strchr(p + 1, ' ');
         if (ptr)
            *ptr = '\0';

         if (strlen(p + 1) < 1)
            printf("LISTEN_IP value %s invalid\n", p + 1);
         else
         {
            memset(LISTEN_IP, '\0', sizeof(LISTEN_IP));
            strncpy(LISTEN_IP, p + 1, IP_SIZE);
            printf("LISTEN_IP=%s\n",LISTEN_IP);
            params ++;
         }
      }
      else
      if (strcmp(inbuf,"LISTEN_PORT") == 0)
      {
         LISTEN_PORT = atoi(p + 1);
         printf("LISTEN_PORT=%d\n",LISTEN_PORT);
         params ++;
      }
      else
      if (strcmp(inbuf,"COMMAND_PORT") == 0)
      {
         COMMAND_PORT = atoi(p + 1);
         printf("COMMAND_PORT=%d\n",COMMAND_PORT);
         params ++;
      }
      else
      if (strcmp(inbuf,"MAX_USERS") == 0)
      {
         MAX_USERS = atoi(p + 1);
         printf("MAX_USERS=%d\n",MAX_USERS);
         params ++;
      }
      else
      if (strcmp(inbuf,"MAX_OTHER_USERS") == 0)
      {
         MAX_OTHER_USERS = atoi(p + 1); 
         printf("MAX_OTHER_USERS=%d\n",MAX_OTHER_USERS);
         params ++; 
      }
      else
      if (strcmp(inbuf,"STATUS_FILE") == 0)
      {
         memset(STATUS_FILE, '\0', sizeof(STATUS_FILE));
         strncpy(STATUS_FILE, p + 1,FILENAME_MAX);
         printf("STATUS_FILE=%s\n",STATUS_FILE);
         params ++;
      }
      else
      if (strcmp(inbuf,"USERS") == 0)
      {
         memset(USERS, '\0', sizeof(USERS));
         strncpy(USERS, p + 1, FILENAME_MAX);
         printf("USERS=%s\n",USERS);
         params ++;
      }
      else
      if (strcmp(inbuf,"BLOCKS") == 0)
      {
         memset(BLOCKS, '\0', sizeof(BLOCKS));
         strncpy(BLOCKS, p + 1, FILENAME_MAX);
         printf("BLOCKS=%s\n",BLOCKS);
         params ++;
      }
      else
      if (strcmp(inbuf,"QSO_DETAILS") == 0)
      {
         if (*(p + 1) == 'Y')
            QSO_DETAILS = true;
         else
            QSO_DETAILS = false;
         printf("QSO_DETAILS=%c\n", *(p + 1));
         params ++;
      }
   }
   fclose(cnf);

   if (params != valid_params)
   {
      printf("Configuration file %s invalid\n",cfgFile);
      return 1;
   }      

   if (COMMAND_PORT == LISTEN_PORT)
   {
      printf("Error: COMMAND_PORT must be different from LISTEN_PORT\n");
      return 1;
   }
   return 0;
}

/* Send heartbeat to repeaters, ... */
static void check_heartbeat()
{
   a_user_list_type::iterator pos;
   a_user_list_type::iterator temp_pos = a_user_list.end();
   struct a_user *a_user_ptr;
   blocks_type::iterator block_pos;
   char dropped = ' ';

   for (pos = a_user_list.begin(); pos != a_user_list.end(); pos++) 
   {
      a_user_ptr = (struct a_user *)pos->second;

      block_pos = blocks.find(a_user_ptr->call);
      
      if (block_pos == blocks.end())  /* not blocked */
      {
         sendto(srv_sock, OWNER, strlen(OWNER) + 1,
                0,(struct sockaddr *)&(a_user_ptr->sin),
                sizeof(struct sockaddr_in));

         if (a_user_ptr->countdown >= 0)
            a_user_ptr->countdown --;

         if (a_user_ptr->countdown < 0)
         {
            dropped = 't'; // timeout
            temp_pos = pos;
         }
      }
      else
      {
         dropped = 'b'; // blocked
         temp_pos = pos;
      }
   }

   if (temp_pos != a_user_list.end())
   {
      a_user_ptr = (struct a_user *)temp_pos->second;

      printf("call=%s %s, removing %s\n",
              a_user_ptr->call,
              (dropped == 't')?"timeout":"blocked",
              temp_pos->first.c_str());

      free(temp_pos->second);
      temp_pos->second = NULL;
      a_user_list.erase(temp_pos);

      print_links_file();
   }

   return;
}

static void print_version()
{
   char buf[24];

   snprintf(buf, 23, "%s\n", VERSION);
   sendto(cmd_sock, buf, strlen(buf),
          0, (struct sockaddr *)&fromCmd,
          sizeof(struct sockaddr_in));
}

/* Print connected users */
static void print_users()
{
   a_user_list_type::iterator pos;
   struct a_user *a_user_ptr;

   inbound_type::iterator pos2;
   inbound *inbound_ptr;

   struct tm tm;

   char buf[256];
   for (pos = a_user_list.begin(); pos != a_user_list.end(); pos++)
   {
      a_user_ptr = (struct a_user *)pos->second; 
      localtime_r(&(a_user_ptr->connect_time),&tm);
      snprintf(buf, 255, "%s,%s,REPEATER,%02d%02d%02d,%02d:%02d:%02d,%s\n", 
               a_user_ptr->call,
               pos->first.c_str(),
               tm.tm_mon+1,tm.tm_mday,tm.tm_year % 100,
               tm.tm_hour,tm.tm_min,tm.tm_sec,
               a_user_ptr->isMute?"Muted":"notMuted");
     
      sendto(cmd_sock, buf, strlen(buf), 
             0, (struct sockaddr *)&fromCmd,
             sizeof(struct sockaddr_in));
   }

   for (pos2 = inbound_list.begin(); pos2 != inbound_list.end(); pos2++)
   {
      inbound_ptr = (inbound *)pos2->second;
      localtime_r(&(inbound_ptr->connect_time),&tm);
      snprintf(buf, 255, "%s,%s,%s(%.8s),%02d%02d%02d,%02d:%02d:%02d,%s\n",
               inbound_ptr->call,
               pos2->first.c_str(),
               inbound_ptr->is_ref?"REPEATER":"DONGLE",
               inbound_ptr->serial,
               tm.tm_mon+1,tm.tm_mday,tm.tm_year % 100,
               tm.tm_hour,tm.tm_min,tm.tm_sec,
               inbound_ptr->isMute?"Muted":"notMuted");
    
      sendto(cmd_sock, buf, strlen(buf),
             0, (struct sockaddr *)&fromCmd,
             sizeof(struct sockaddr_in));
   }
   return;
}

/* Mute or unmute all users */
static void mute_users(bool mute)
{
   a_user_list_type::iterator pos;
   struct a_user *a_user_ptr;

   inbound_type::iterator pos2;
   inbound *inbound_ptr;

   for (pos = a_user_list.begin(); pos != a_user_list.end(); pos++)
   {
      a_user_ptr = (struct a_user *)pos->second;
      a_user_ptr->isMute = mute;
   }

   for (pos2 = inbound_list.begin(); pos2 != inbound_list.end(); pos2++)
   {
      inbound_ptr = (inbound *)pos2->second;
      inbound_ptr->isMute = mute;
   }
   return;
}


/* mute or unmute a user by callsign */
static bool mute_call(char *call, bool mute)
{
   bool found = false;
   a_user_list_type::iterator pos;
   struct a_user *a_user_ptr;

   inbound_type::iterator pos2;
   inbound *inbound_ptr;

   for (pos = a_user_list.begin(); pos != a_user_list.end(); pos++)
   {
      a_user_ptr = (struct a_user *)pos->second;
      if (strcmp(a_user_ptr->call, call) == 0)
      {
         found = true;
         a_user_ptr->isMute = mute;
      }
   }

   for (pos2 = inbound_list.begin(); pos2 != inbound_list.end(); pos2++)
   {
      inbound_ptr = (inbound *)pos2->second;
      if (strcmp(inbound_ptr->call, call) == 0)
      {
         found = true;
         inbound_ptr->isMute = mute;
      }
   }

   return found;
}

/* print the links on the screen */
static void print_links_screen()
{
   char buf[256];
   struct tm tm;

   inbound_type::iterator inbound_pos;
   inbound *inbound_ptr;

   a_user_list_type::iterator pos;
   struct a_user *a_user_ptr;
   short int i, j;
   char from_mod = ' ';

   for (pos = a_user_list.begin(); pos != a_user_list.end(); pos++)
   {
      a_user_ptr = (struct a_user *)pos->second;
      for (i = 0; i < 26; i++)
      {
         for (j = 0; j < 25; j++)
         {
            if (a_user_ptr->rpt_mods[i][j] != '\0')
            {
               if (i == 0)
                  from_mod = 'A';
               else
               if (i == 1)
                  from_mod = 'B';
               else
               if (i == 2)
                  from_mod = 'C';
               else
               if (i == 3)
                  from_mod = 'D';
               else
               if (i == 4)
                  from_mod = 'E';
			  else
               if (i == 5)
                  from_mod = 'F';
			  else
               if (i == 6)
                  from_mod = 'G';
			  else
               if (i == 7)
                  from_mod = 'H';
			  else
               if (i == 8)
                  from_mod = 'I';
			  else
               if (i == 9)
                  from_mod = 'J';
			  else
               if (i == 10)
                  from_mod = 'K';
			  else
               if (i == 11)
                  from_mod = 'L';
			  else
               if (i == 12)
                  from_mod = 'M';
			  else
               if (i == 13)
                  from_mod = 'N';
			  else
               if (i == 14)
                  from_mod = 'O';
			  else
               if (i == 15)
                  from_mod = 'P';
			  else
               if (i == 16)
                  from_mod = 'Q';
			  else
               if (i == 17)
                  from_mod = 'R';
			  else
               if (i == 18)
                  from_mod = 'S';
			  else
               if (i == 19)
                  from_mod = 'T';
			  else
               if (i == 20)
                  from_mod = 'U';
			  else
               if (i == 21)
                  from_mod = 'V';
			  else
               if (i == 22)
                  from_mod = 'W';
			  else
               if (i == 23)
                  from_mod = 'X';
			  else
               if (i == 24)
                  from_mod = 'Y';
			  else
               if (i == 25)
                  from_mod = 'Z';

               localtime_r(&(a_user_ptr->link_time[i][j]),&tm);
               snprintf(buf, 255, "%c,%s,%c,%s,%02d%02d%02d,%02d:%02d:%02d\n",
                        from_mod,
                        a_user_ptr->call,
                        a_user_ptr->rpt_mods[i][j],
                        pos->first.c_str(),
                        tm.tm_mon+1,tm.tm_mday,tm.tm_year % 100,
                        tm.tm_hour,tm.tm_min,tm.tm_sec);

               sendto(cmd_sock, buf, strlen(buf),
                      0, (struct sockaddr *)&fromCmd,
                      sizeof(struct sockaddr_in));
            }
         }
      }
   }

   for (inbound_pos = inbound_list.begin(); inbound_pos != inbound_list.end(); inbound_pos++)
   {
      inbound_ptr = (inbound *)inbound_pos->second;
      for (i = 0; i < 26; i++)
      {
         if (inbound_ptr->links[i] != ' ')
         {
            if (i == 0)
               from_mod = 'A';
            else
            if (i == 1)
               from_mod = 'B';
            else
            if (i == 2)
               from_mod = 'C';
            else
            if (i == 3)
               from_mod = 'D';
		   else
            if (i == 4)
               from_mod = 'E';
		   else
            if (i == 5)
               from_mod = 'F';
		   else
            if (i == 6)
               from_mod = 'G';
		   else
            if (i == 7)
               from_mod = 'H';
		   else
            if (i == 8)
               from_mod = 'I';
		   else
            if (i == 9)
               from_mod = 'J';
		   else
            if (i == 10)
               from_mod = 'K';
		   else
            if (i == 11)
               from_mod = 'L';
		   else
            if (i == 12)
               from_mod = 'M';
		   else
            if (i == 13)
               from_mod = 'N';
		   else
            if (i == 14)
               from_mod = 'O';
		   else
            if (i == 15)
               from_mod = 'P';
		   else
            if (i == 16)
               from_mod = 'Q';
		   else
            if (i == 17)
               from_mod = 'R';
		   else
            if (i == 18)
               from_mod = 'S';
		   else
            if (i == 19)
               from_mod = 'T';
		   else
            if (i == 20)
               from_mod = 'U';
		   else
            if (i == 21)
               from_mod = 'V';
		   else
            if (i == 22)
               from_mod = 'W';
		   else
            if (i == 23)
               from_mod = 'X';
		   else
            if (i == 24)
               from_mod = 'Y';
            else
               from_mod = 'Z';

            localtime_r(&(inbound_ptr->connect_time), &tm);
            snprintf(buf, 255, "%c,%s,%c,%s,%02d%02d%02d,%02d:%02d:%02d\n",
                  from_mod,
                  inbound_ptr->call,
                  inbound_ptr->links[i],
                  inbound_pos->first.c_str(),
                  tm.tm_mon+1,tm.tm_mday,tm.tm_year % 100,
                  tm.tm_hour,tm.tm_min,tm.tm_sec);

            sendto(cmd_sock, buf, strlen(buf),
                   0, (struct sockaddr *)&fromCmd,
                   sizeof(struct sockaddr_in));
         }
      }
   }
}

/* Print links into the STATUS file */
static void print_links_file()
{
   inbound_type::iterator inbound_pos;
   inbound *inbound_ptr;

   a_user_list_type::iterator pos;
   struct a_user *a_user_ptr;
   struct tm tm;
   short int i, j;
   char from_mod = ' ';

   statusfp = fopen(STATUS_FILE, "w");
   if (!statusfp)
         printf("Failed to create status file %s\n", STATUS_FILE);
   else
   {
      setvbuf(statusfp, (char *)NULL, _IOLBF, 0);
      for (pos = a_user_list.begin(); pos != a_user_list.end(); pos++)
      {
         a_user_ptr = (struct a_user *)pos->second;
         for (i = 0; i < 26; i++)
         {
            for (j = 0; j < 25; j++)
            {
               if (a_user_ptr->rpt_mods[i][j] != '\0')
               {
                  if (i == 0)
					from_mod = 'A';
				   else
				   if (i == 1)
					  from_mod = 'B';
				   else
				   if (i == 2)
					  from_mod = 'C';
				   else
				   if (i == 3)
					  from_mod = 'D';
				   else
				   if (i == 4)
					  from_mod = 'E';
				  else
				   if (i == 5)
					  from_mod = 'F';
				  else
				   if (i == 6)
					  from_mod = 'G';
				  else
				   if (i == 7)
					  from_mod = 'H';
				  else
				   if (i == 8)
					  from_mod = 'I';
				  else
				   if (i == 9)
					  from_mod = 'J';
				  else
				   if (i == 10)
					  from_mod = 'K';
				  else
				   if (i == 11)
					  from_mod = 'L';
				  else
				   if (i == 12)
					  from_mod = 'M';
				  else
				   if (i == 13)
					  from_mod = 'N';
				  else
				   if (i == 14)
					  from_mod = 'O';
				  else
				   if (i == 15)
					  from_mod = 'P';
				  else
				   if (i == 16)
					  from_mod = 'Q';
				  else
				   if (i == 17)
					  from_mod = 'R';
				  else
				   if (i == 18)
					  from_mod = 'S';
				  else
				   if (i == 19)
					  from_mod = 'T';
				  else
				   if (i == 20)
					  from_mod = 'U';
				  else
				   if (i == 21)
					  from_mod = 'V';
				  else
				   if (i == 22)
					  from_mod = 'W';
				  else
				   if (i == 23)
					  from_mod = 'X';
				  else
				   if (i == 24)
					  from_mod = 'Y';
				  else
				   if (i == 25)
					  from_mod = 'Z';
				 

               localtime_r(&(a_user_ptr->link_time[i][j]),&tm);
               fprintf(statusfp, "%c,%s,%c,%s,%02d%02d%02d,%02d:%02d:%02d\n",
                       from_mod,
                       a_user_ptr->call, 
                       a_user_ptr->rpt_mods[i][j],
                       pos->first.c_str(),
                       tm.tm_mon+1,tm.tm_mday,tm.tm_year % 100,
                       tm.tm_hour,tm.tm_min,tm.tm_sec);
               }
            }
         }
      }

      for (inbound_pos = inbound_list.begin(); inbound_pos != inbound_list.end(); inbound_pos++)
      {
         inbound_ptr = (inbound *)inbound_pos->second;
         for (i = 0; i < 26; i++)
         {
            if (inbound_ptr->links[i] != ' ')
            {
				if (i == 0)
				   from_mod = 'A';
				else
				if (i == 1)
				   from_mod = 'B';
				else
				if (i == 2)
				   from_mod = 'C';
				else
				if (i == 3)
				   from_mod = 'D';
			   else
				if (i == 4)
				   from_mod = 'E';
			   else
				if (i == 5)
				   from_mod = 'F';
			   else
				if (i == 6)
				   from_mod = 'G';
			   else
				if (i == 7)
				   from_mod = 'H';
			   else
				if (i == 8)
				   from_mod = 'I';
			   else
				if (i == 9)
				   from_mod = 'J';
			   else
				if (i == 10)
				   from_mod = 'K';
			   else
				if (i == 11)
				   from_mod = 'L';
			   else
				if (i == 12)
				   from_mod = 'M';
			   else
				if (i == 13)
				   from_mod = 'N';
			   else
				if (i == 14)
				   from_mod = 'O';
			   else
				if (i == 15)
				   from_mod = 'P';
			   else
				if (i == 16)
				   from_mod = 'Q';
			   else
				if (i == 17)
				   from_mod = 'R';
			   else
				if (i == 18)
				   from_mod = 'S';
			   else
				if (i == 19)
				   from_mod = 'T';
			   else
				if (i == 20)
				   from_mod = 'U';
			   else
				if (i == 21)
				   from_mod = 'V';
			   else
				if (i == 22)
				   from_mod = 'W';
			   else
				if (i == 23)
				   from_mod = 'X';
			   else
				if (i == 24)
				   from_mod = 'Y';
				else
				   from_mod = 'Z';

               localtime_r(&(inbound_ptr->connect_time), &tm);
               fprintf(statusfp, "%c,%s,%c,%s,%02d%02d%02d,%02d:%02d:%02d\n",
                  from_mod,
                  inbound_ptr->call,
                  inbound_ptr->links[i],
                  inbound_pos->first.c_str(),
                  tm.tm_mon+1,tm.tm_mday,tm.tm_year % 100,
                  tm.tm_hour,tm.tm_min,tm.tm_sec);
            }
         }
      }
      fclose(statusfp);
   }
}

/* print blocked users */
static void print_blocks()
{
   blocks_type::iterator pos;
   char buf[32];

   for (pos = blocks.begin(); pos != blocks.end(); pos++)
   {
      snprintf(buf, 31, "[%s]\n", pos->c_str()); 
      sendto(cmd_sock, buf, strlen(buf),
             0, (struct sockaddr *)&fromCmd,
             sizeof(struct sockaddr_in));
   }
}

/* signal catching function */
static void sigCatch(int signum)
{
   /* do NOT do any serious work here */
   /* Just get out of its processing loop */

      if ((signum == SIGTERM) ||
       (signum == SIGINT))
         keep_running = false;
   return;
}

/* Open server socket for dvap, dvtool ... */
static int ref_open()
{
   struct sockaddr_in sin;

   ref_sock = socket(PF_INET,SOCK_DGRAM,0);
   if (ref_sock == -1)
   {
      printf("Failed to create ref socket,errno=%d\n",errno);
      return 1;
   }
   memset(&sin,0,sizeof(struct sockaddr_in));
   sin.sin_family = AF_INET;
   sin.sin_port = htons(20001);
   sin.sin_addr.s_addr = inet_addr(LISTEN_IP);

   if (bind(ref_sock,(struct sockaddr *)&sin,sizeof(struct sockaddr_in)) != 0)
   {
      printf("Failed to bind ref socket on IP %s, port 20001, errno=%d\n",
              LISTEN_IP, errno);
      close(ref_sock); ref_sock = -1;
      return 1;
   }
   fcntl(ref_sock,F_SETFL,O_NONBLOCK);

   return 0;
}

/* Open server socket for connected stations */
static int srv_open()
{
   struct sockaddr_in sin;

   srv_sock = socket(PF_INET,SOCK_DGRAM,0);
   if (srv_sock == -1)
   {
      printf("Failed to create server socket,errno=%d\n",errno);
      return 1;
   }
   memset(&sin,0,sizeof(struct sockaddr_in));
   sin.sin_family = AF_INET;
   sin.sin_port = htons(LISTEN_PORT);
   sin.sin_addr.s_addr = inet_addr(LISTEN_IP);

   if (bind(srv_sock,(struct sockaddr *)&sin,sizeof(struct sockaddr_in)) != 0)
   {
      printf("Failed to bind server socket on IP %s, port %d, errno=%d\n",
              LISTEN_IP, LISTEN_PORT,errno);
      close(srv_sock); srv_sock = -1;
      return 1;
   }
   fcntl(srv_sock,F_SETFL,O_NONBLOCK); 

   return 0;
}

/* 
   Open command socket for handling commands from the shell.
   Use netcat as a client to send commands to this reflector
*/
static int cmd_open()
{
   struct sockaddr_in sin;

   cmd_sock = socket(PF_INET,SOCK_DGRAM,0);
   if (cmd_sock == -1)
   {
      printf("Failed to create cmd socket,errno=%d\n",errno);
      return 1;
   }
   memset(&sin,0,sizeof(struct sockaddr_in));
   sin.sin_family = AF_INET;
   sin.sin_port = htons(COMMAND_PORT);
   sin.sin_addr.s_addr = inet_addr(LISTEN_IP);

   if (bind(cmd_sock,(struct sockaddr *)&sin,sizeof(struct sockaddr_in)) != 0)
   {
      printf("Failed to bind cmd socket on IP %s, port %d, errno=%d\n",
              LISTEN_IP, COMMAND_PORT,errno);
      close(cmd_sock); cmd_sock = -1;
      return 1;
   }
   fcntl(cmd_sock,F_SETFL,O_NONBLOCK);

   return 0;
}

/* Process the shell command */
static void handle_cmd(char *buf)
{
   char *p = NULL;
   char call[CALL_SIZE + 1];
   char cmd[25];
   char cmdprompt[3];
   unsigned short i;
   char nak[8];
   char const *timeoutmsg = "TIMEOUT\n";
   time_t timenow;
   double time_since_unlock = 0;

   char *ptr_cmd = NULL;
   char *ptr_call = NULL;
   const char *delim = " ";

   blocks_type::iterator pos;

   int rc = -1;

   nak[0] = '\0';

   p = strchr(buf, '\r');
   if (p)
      *p = '\0';

   p = strchr(buf, '\n');
   if (p)
      *p = '\0';

   if (strlen(buf) < 2)
      return;

   time(&timenow);  /* Re-lock command processor 60 sec after password entered */
   time_since_unlock = difftime(timenow,unlocktime);
   if (time_since_unlock > 60 && pwunlock)
   {
      pwunlock = 0;
      sendto(cmd_sock,timeoutmsg,9,
       0, (struct sockaddr *)&fromCmd,
       sizeof(struct sockaddr_in));
   }

   printf("Received command [%s] from [%s]\n", buf, inet_ntoa(fromCmd.sin_addr));

   if (strcmp(buf, "lk") == 0)  /* lock command processor */
   {
      pwunlock = 0;
      strcpy(nak,"*");
   }
   else
   if (strcmp(buf, "sh") == 0 && pwunlock)  /* shutdown reflector */
      keep_running = false;
   else
   if (strcmp(buf, "mu") == 0 && pwunlock) /* mute users */
      mute_users(true);
   else
   if (strcmp(buf, "uu") == 0 && pwunlock) /* un-mute users */
      mute_users(false);
   else
   if (strcmp(buf, "qsoy") == 0 && pwunlock)  /* QSO_DETAILS yes */
      QSO_DETAILS = true;
   else
   if (strcmp(buf, "qson") == 0 && pwunlock)
      QSO_DETAILS = false;       /* QSO_DETAILS no */
   else
   if (strcmp(buf, "pu") == 0)   /* print users */
      print_users();
   else
   if (strcmp(buf, "pv") == 0)   /* print version */
      print_version();
   else
   if (strcmp(buf, "pb") == 0 && pwunlock)   /* print blocked users */
      print_blocks();
   else
   if (strcmp(buf, "pl") == 0)   /* print links */
      print_links_screen();
   else
   if (strcmp(buf, "upd") == 0 && pwunlock)  /* reload the database */
   {
      rc = open_users(USERS);
      if (rc != 0)
         printf("Failed to reload %s\n", USERS);
   }
   else
   {
      ptr_cmd = strtok(buf, delim);
      ptr_call = strtok(NULL, delim);

      if (!ptr_cmd || !ptr_call)
      {
         printf("Missing command parameters\n");
         strcpy(nak, "NAK   \n");
      }
      else
      if (strlen(ptr_cmd) > 3)
      {
         printf("Invalid command length\n");
         strcpy(nak, "NAK   \n");
      }
      else
      {
         strcpy(cmd, ptr_cmd);
         if (strlen(ptr_call) <= CALL_SIZE)
         {
            strcpy(call, ptr_call);

            if (strlen(call) < CALL_SIZE)
            {
               for (i = strlen(call); i < CALL_SIZE; i++)
                  call[i] = ' ';
               call[CALL_SIZE] = '\0';
            }
            for (i = 0; i < CALL_SIZE; i++)
            {
               if (call[i] == '_')
                  call[i] = ' ';
            }

            if (strcmp(cmd, "ab") == 0 && pwunlock)   /* ab <CALLSIGN>    this will block the callsign */
            {
               pos = blocks.find(call);
               if (pos != blocks.end())
               {
                  printf("Call %s already blocked\n", call);
                  strcpy(nak, "NAK   \n");
               }
               else
               if (blocks.insert(call).second)
                  printf("call %s is now blocked\n", call);
               else
               {
                  printf("Failed to block call %s\n",call);
                  strcpy(nak, "NAK   \n");
               }
            }
            else
            if (strcmp(cmd, "rb") == 0 && pwunlock)   /* rb  <CALLSIGN>     this will remove the block on the callsign */
            {
               if (blocks.erase(call) > 0)
                  printf("call %s is now unblocked\n", call);
               else
               {
                  printf("Could not find blocked call %s\n", call);
                  strcpy(nak, "NAK   \n");
               }
            }
            else
            if (strcmp(cmd, "mc") == 0 && pwunlock) /* mute a user by callsign */
            {
               if (!mute_call(call, true))
                  strcpy(nak, "NAK   \n");
            }
            else
            if (strcmp(cmd, "uc") == 0 && pwunlock) /* un-mute a user by callsign */
            {
               if (!mute_call(call, false))
                  strcpy(nak, "NAK   \n");
            }
            else
            if (strcmp(cmd, "lrf") == 0 && pwunlock)  /* link to ref */
            {
               rc = link_to_ref(call);
               if (rc != 0)
                  strcpy(nak, "NAK   \n");
            }
            else
            if (strcmp(cmd, "ul") == 0) /* unlock command processor for commands */
            {
               if (strncmp(call,passwd,strlen(passwd)) == 0)
               {
                  pwunlock = 1;
                  time(&unlocktime);
               }
            }
            else
            {
               printf("Unknown command or bad parameter\n");
               strcpy(nak, "NAK\n");
            }
         }
      }
   }

   if (nak[0] == '\0' && pwunlock)
      time(&unlocktime);  /*  reset cmd timer  */

   if (nak[0] == '\0')
      strcpy(nak, "OK    \n");
   else
   if (nak[0] == '*')
      strcpy(nak, "OK-LKD\n");

   if (!pwunlock && nak[2] != '-')
      strcpy(nak, "LOCKED\n");

   sendto(cmd_sock,nak,7,
          0, (struct sockaddr *)&fromCmd,
          sizeof(struct sockaddr_in));

   if(pwunlock)
   {
      strcpy(cmdprompt,"$ ");
      sendto(cmd_sock,cmdprompt,2,
          0, (struct sockaddr *)&fromCmd,
          sizeof(struct sockaddr_in));
   }

   return;
}

/* Reflector is running here */
static void runit()
{
   short i,j,k,n;
   int recvlen;
   int recvlen2;
   int recvlen3;
   socklen_t fromlen;
   char *p = NULL;
   int call_valid = REG_NOERROR;

   bool allowed_to_connect = true;
   //bool deleted = false;
   char search_value[MAXHOSTNAMELEN + 7];

   char a_call[CALL_SIZE + 1];
   char a_call2[CALL_SIZE + 1];
   char an_ip[IP_SIZE + 1];
   char a_port[7];

   call_ip_type::iterator pos;

   struct a_user *a_user_ptr;
   a_user_list_type::iterator user_pos;
   struct a_user *a_user_ptr2;
   a_user_list_type::iterator user_pos2;
   pair<a_user_list_type::iterator,bool> user_insert_pair;
   blocks_type::iterator block_pos;

   inbound *inbound_ptr;
   inbound_type::iterator inbound_pos;
   inbound *inbound_ptr2;
   inbound_type::iterator inbound_pos2;
   pair<inbound_type::iterator,bool> inbound_insert_pair;
   bool found = false;

   char source_mod = ' ';
   int max_nfds = 0;

   char tmp1[CALL_SIZE + 1];
   char tmp2[32]; // 8 for rpt1 + 24 for time_t in string format
   dt_lh_type::iterator dt_lh_pos;
   dt_lh_type::reverse_iterator r_dt_lh_pos;

   int rc;

   char reply_to_xrf[11]; 

   /* faster select */
   if (srv_sock > max_nfds)
      max_nfds = srv_sock;
   if (cmd_sock > max_nfds)
      max_nfds = cmd_sock;
   if (ref_sock > max_nfds)
      max_nfds = ref_sock;

   printf("srv=%d, cmd=%d, ref=%d, MAX+1=%d\n", 
            srv_sock,cmd_sock,ref_sock, max_nfds + 1);

   time(&HBinterStart);
   time(&inboundStart);

   time(&check_rcd_time);

   while (keep_running)
   {
      /* send heartbeat to connected dvap, dvtool ... */
      time(&tNow);
      if ((tNow - inboundStart) > 0)
      {
         send_heartbeat();
         inboundStart = tNow;
      } 

      /* send a heartbeat to linked gateways */
      time(&tNow);
      if ((tNow - HBinterStart) > 0)
      {
         check_heartbeat();
         HBinterStart = tNow;
      }

      time(&tNow);

      if ((tNow - check_rcd_time) > 3)
      {
         for (rcd_pos = rcd_list.begin(); rcd_pos != rcd_list.end(); rcd_pos ++)
         {
            an_rcd = (struct rcd *)rcd_pos->second;
            if (!an_rcd->locked)
            {
               if ((tNow - an_rcd->ts) > 1)
               {
                  /***
                  if (an_rcd->recvlen == 56)
                     printf("Removing playback streamid %d,%d\n", an_rcd->data[0][12], an_rcd->data[0][13]);
                  else
                     printf("Removing playback streamid %d,%d\n", an_rcd->data[0][14], an_rcd->data[0][15]);
                  ***/
                  free(rcd_pos->second);
                  rcd_pos->second = NULL;
                  rcd_list.erase(rcd_pos);
               }
            }
         } 
         check_rcd_time = tNow;
      }

      /* wait 20 ms max */
      FD_ZERO(&fdset);
      FD_SET(srv_sock,&fdset);
      FD_SET(cmd_sock,&fdset);
      FD_SET(ref_sock,&fdset);
      tv.tv_sec = 0;
      tv.tv_usec = 20000; /* 20 ms */
      (void)select(max_nfds + 1,&fdset,0,0,&tv);

      /* process shell commands */
      if (FD_ISSET(cmd_sock, &fdset))
      {
         fromlen = sizeof(struct sockaddr_in);
         recvlen2 = recvfrom(cmd_sock,(char *)readBuffer,READBUFFER_SIZE,
                            0,(struct sockaddr *)&fromCmd,&fromlen);
         if (recvlen2 > 0)
         {
            readBuffer[recvlen2 - 1] = '\0';
            handle_cmd((char *)readBuffer);
         }

         FD_CLR (cmd_sock,&fdset);
      }

      /*  process input from dvap, dvtool */
      if (FD_ISSET(ref_sock, &fdset))
      {
         fromlen = sizeof(struct sockaddr_in);
         recvlen3 = recvfrom(ref_sock,(char *)refbuf,READBUFFER_SIZE,
                            0,(struct sockaddr *)&fromInbound,&fromlen);

         strncpy(an_ip, inet_ntoa(fromInbound.sin_addr),IP_SIZE);
         an_ip[IP_SIZE] = '\0';
         snprintf(a_port, 6, "%d", ntohs(fromInbound.sin_port));
         a_port[26] = '\0';

         /* LH */
         if ((recvlen3 == 4) &&
             (refbuf[0] == 4) &&
             (refbuf[1] == 192) &&
             (refbuf[2] == 7) &&
             (refbuf[3] == 0))
         {
            unsigned short j_idx = 0;
            unsigned short k_idx = 0;
            unsigned char tmp[2];

            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               /* header is 10 bytes */

               /* reply type */
               refbuf[2] = 7;
               refbuf[3] = 0;

               time(&tNow);
               memcpy((char *)refbuf + 6, (char *)&tNow, sizeof(time_t));

               for (r_dt_lh_pos = dt_lh_list.rbegin(); r_dt_lh_pos != dt_lh_list.rend();  r_dt_lh_pos++)
               {
                  /* each entry has 24 bytes */

                  /* start at position 10 to bypass the header */
                  strcpy((char *)refbuf + 10 + (24 * j_idx), r_dt_lh_pos->second.c_str());
                  p = strchr((char *)r_dt_lh_pos->first.c_str(), '=');
                  if (p)
                  {
                     memcpy((char *)refbuf + 18 + (24 * j_idx), p + 1, 8);
                     *p = '\0';
                     tNow = atol(r_dt_lh_pos->first.c_str());
                     *p = '=';
                     memcpy((char *)refbuf + 26 + (24 * j_idx), &tNow, sizeof(time_t));
                  }
                  else
                  {
                     memcpy((char *)refbuf + 18 + (24 * j_idx), "ERROR   ", 8);
                     time(&tNow);
                     memcpy((char *)refbuf + 26 + (24 * j_idx), &tNow, sizeof(time_t));
                  }

                  refbuf[30 + (24 * j_idx)] = 0;
                  refbuf[31 + (24 * j_idx)] = 0;
                  refbuf[32 + (24 * j_idx)] = 0;
                  refbuf[33 + (24 * j_idx)] = 0;

                  j_idx++;

                  /* process 39 entries at a time */
                  if (j_idx == 39)
                  {
                     /* 39 * 24 = 936 + 10 header = 946 */
                     refbuf[0] = 0xb2;
                     refbuf[1] = 0xc3;

                     /* 39 entries */
                     refbuf[4] = 0x27;
                     refbuf[26] = 0x00;

                     sendto(ref_sock,(char *)refbuf,946,0,
                            (struct sockaddr *)&fromInbound,
                            sizeof(struct sockaddr_in));

                     j_idx = 0;
                  }
               }

               if (j_idx != 0)
               {
                  k_idx = 10 + (j_idx * 24);
                  memcpy(tmp, (char *)&k_idx, 2);
                  refbuf[0] = tmp[0];
                  refbuf[1] = tmp[1] | 0xc0;

                  memcpy(tmp, (char *)&j_idx, 2);
                  refbuf[4] = tmp[0];
                  refbuf[26] = tmp[1];

                 sendto(ref_sock,(char *)refbuf, k_idx, 0,
                         (struct sockaddr *)&fromInbound,
                         sizeof(struct sockaddr_in));
               }
            }
         }
         else 
         /* linked repeaters request */
         if ((recvlen3 == 4) &&
             (refbuf[0] == 4) &&
             (refbuf[1] == 192) &&
             (refbuf[2] == 5) &&
             (refbuf[3] == 0))
         {
            unsigned short i_idx = 0;
            unsigned short j_idx = 0;
            unsigned short k_idx = 0;
            short module_idx;
            unsigned char tmp[2];
            unsigned short total = 0;

            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               /* header is 8 bytes */

               /* reply type */
               refbuf[2] = 5;
               refbuf[3] = 1;

               total =  a_user_list.size();
               memcpy(tmp, (char *)&total, 2);
               refbuf[6] = tmp[0];
               refbuf[7] = tmp[1];

               for (user_pos = a_user_list.begin(), i_idx = 0; user_pos != a_user_list.end();  user_pos++, i_idx++)
               {
                  /* each entry has 20 bytes */
                  a_user_ptr = (struct a_user *)user_pos->second;
                  for (module_idx = 0; module_idx < 26; module_idx++)
                  { 
                     for (k = 0; k < 25; k++)
                     {
                        if (a_user_ptr->rpt_mods[module_idx][k] != '\0')
                        {
                           if (module_idx == 0)
                              refbuf[8 + (20 * j_idx)] = 'A';
                           else
                           if (module_idx == 1)
                              refbuf[8 + (20 * j_idx)] = 'B';
                           else
                           if (module_idx == 2)
                              refbuf[8 + (20 * j_idx)] = 'C';
                           else
                           if (module_idx == 3)
                              refbuf[8 + (20 * j_idx)] = 'D';
                           else
                           if (module_idx == 4)
                              refbuf[8 + (20 * j_idx)] = 'E';   
						  else
                           if (module_idx == 5)
                              refbuf[8 + (20 * j_idx)] = 'F';  
						  else
                           if (module_idx == 6)
                              refbuf[8 + (20 * j_idx)] = 'G';  
						  else
                           if (module_idx == 7)
                              refbuf[8 + (20 * j_idx)] = 'H';  
						  else
                           if (module_idx == 8)
                              refbuf[8 + (20 * j_idx)] = 'I';  
						  else
                           if (module_idx == 9)
                              refbuf[8 + (20 * j_idx)] = 'J';  
						  else
                           if (module_idx == 10)
                              refbuf[8 + (20 * j_idx)] = 'K';  
						  else
                           if (module_idx == 11)
                              refbuf[8 + (20 * j_idx)] = 'L';  
						  else
                           if (module_idx == 12)
                              refbuf[8 + (20 * j_idx)] = 'M';  
						  else
                           if (module_idx == 13)
                              refbuf[8 + (20 * j_idx)] = 'N';  
						  else
                           if (module_idx == 14)
                              refbuf[8 + (20 * j_idx)] = 'O';  
						  else
                           if (module_idx == 15)
                              refbuf[8 + (20 * j_idx)] = 'P';  
						  else
                           if (module_idx == 16)
                              refbuf[8 + (20 * j_idx)] = 'Q';  
						  else
                           if (module_idx == 17)
                              refbuf[8 + (20 * j_idx)] = 'R';  
						  else
                           if (module_idx == 18)
                              refbuf[8 + (20 * j_idx)] = 'S';  
						  else
                           if (module_idx == 19)
                              refbuf[8 + (20 * j_idx)] = 'T';  
						  else
                           if (module_idx == 20)
                              refbuf[8 + (20 * j_idx)] = 'U';  
						  else
                           if (module_idx == 21)
                              refbuf[8 + (20 * j_idx)] = 'V';  
						  else
                           if (module_idx == 22)
                              refbuf[8 + (20 * j_idx)] = 'W';  
						  else
                           if (module_idx == 23)
                              refbuf[8 + (20 * j_idx)] = 'X';  
						  else
                           if (module_idx == 24)
                              refbuf[8 + (20 * j_idx)] = 'Y';  
						  else
                           if (module_idx == 25)
                              refbuf[8 + (20 * j_idx)] = 'Z';  
                            
                           strcpy((char *)refbuf + 9 + (20 * j_idx), a_user_ptr->call);
                           refbuf[16 + (20 * j_idx)] = a_user_ptr->rpt_mods[module_idx][k];

                           refbuf[17 + (20 * j_idx)] = 0;
                           refbuf[18 + (20 * j_idx)] = 0;
                           refbuf[19 + (20 * j_idx)] = 0;
                           refbuf[20 + (20 * j_idx)] = 0x50;
                           refbuf[21 + (20 * j_idx)] = 0x04;
                           refbuf[22 + (20 * j_idx)] = 0x32;
                           refbuf[23 + (20 * j_idx)] = 0x4d;
                           refbuf[24 + (20 * j_idx)] = 0x9f;
                           refbuf[25 + (20 * j_idx)] = 0xdb;
                           refbuf[26 + (20 * j_idx)] = 0x0e;
                           refbuf[27 + (20 * j_idx)] = 0;

                           j_idx++;

                           if (j_idx == 39)
                           {
                              /* 20 bytes for each user, so 39 * 20 = 780 bytes + 8 bytes header = 788 */
                              refbuf[0] = 0x14;   
                              refbuf[1] = 0xc3;  

                              k_idx = i_idx - 38;
                              memcpy(tmp, (char *)&k_idx, 2);
                              refbuf[4] = tmp[0];
                              refbuf[26] = tmp[1];

                              sendto(ref_sock,(char *)refbuf,788,0,
                                     (struct sockaddr *)&fromInbound,
                                     sizeof(struct sockaddr_in));

                              j_idx = 0;
                           }
                        }
                     }
                  }
               }

               if (j_idx != 0)
               {
                  k_idx = 8 + (j_idx * 20);
                  memcpy(tmp, (char *)&k_idx, 2);
                  refbuf[0] = tmp[0];
                  refbuf[1] = tmp[1] | 0xc0;

                  if (i_idx > j_idx)
                     k_idx = i_idx - j_idx;
                  else
                     k_idx = 0;

                  mempcpy(tmp, (char *)&k_idx, 2);
                  refbuf[4] = tmp[0];
                  refbuf[26] = tmp[1];

                  sendto(ref_sock,(char *)refbuf, 8 + (j_idx * 20), 0,
                         (struct sockaddr *)&fromInbound,
                         sizeof(struct sockaddr_in));
               }

               i_idx = 0;
               j_idx = 0;
               k_idx = 0;
               for (inbound_pos = inbound_list.begin(), i_idx = 0; inbound_pos != inbound_list.end();  inbound_pos++, i_idx++)
               {
                  /* each entry has 20 bytes */
                  inbound_ptr = (inbound *)inbound_pos->second;
                  if (inbound_ptr->is_ref)
                  {
                     for (module_idx = 0; module_idx < 16; module_idx++)
                     {
                        if (inbound_ptr->links[module_idx] != ' ')
                        {
                           if (module_idx == 0)
                              refbuf[8 + (20 * j_idx)] = 'A';
                           else
                           if (module_idx == 1)
                              refbuf[8 + (20 * j_idx)] = 'B';
                           else
                           if (module_idx == 2)
                              refbuf[8 + (20 * j_idx)] = 'C';
                           else
                           if (module_idx == 3)
                              refbuf[8 + (20 * j_idx)] = 'D';
                           else
                           if (module_idx == 4)
                              refbuf[8 + (20 * j_idx)] = 'E';   
						  else
                           if (module_idx == 5)
                              refbuf[8 + (20 * j_idx)] = 'F';  
						  else
                           if (module_idx == 6)
                              refbuf[8 + (20 * j_idx)] = 'G';  
						  else
                           if (module_idx == 7)
                              refbuf[8 + (20 * j_idx)] = 'H';  
						  else
                           if (module_idx == 8)
                              refbuf[8 + (20 * j_idx)] = 'I';  
						  else
                           if (module_idx == 9)
                              refbuf[8 + (20 * j_idx)] = 'J';  
						  else
                           if (module_idx == 10)
                              refbuf[8 + (20 * j_idx)] = 'K';  
						  else
                           if (module_idx == 11)
                              refbuf[8 + (20 * j_idx)] = 'L';  
						  else
                           if (module_idx == 12)
                              refbuf[8 + (20 * j_idx)] = 'M';  
						  else
                           if (module_idx == 13)
                              refbuf[8 + (20 * j_idx)] = 'N';  
						  else
                           if (module_idx == 14)
                              refbuf[8 + (20 * j_idx)] = 'O';  
						  else
                           if (module_idx == 15)
                              refbuf[8 + (20 * j_idx)] = 'P';  
						  else
                           if (module_idx == 16)
                              refbuf[8 + (20 * j_idx)] = 'Q';  
						  else
                           if (module_idx == 17)
                              refbuf[8 + (20 * j_idx)] = 'R';  
						  else
                           if (module_idx == 18)
                              refbuf[8 + (20 * j_idx)] = 'S';  
						  else
                           if (module_idx == 19)
                              refbuf[8 + (20 * j_idx)] = 'T';  
						  else
                           if (module_idx == 20)
                              refbuf[8 + (20 * j_idx)] = 'U';  
						  else
                           if (module_idx == 21)
                              refbuf[8 + (20 * j_idx)] = 'V';  
						  else
                           if (module_idx == 22)
                              refbuf[8 + (20 * j_idx)] = 'W';  
						  else
                           if (module_idx == 23)
                              refbuf[8 + (20 * j_idx)] = 'X';  
						  else
                           if (module_idx == 24)
                              refbuf[8 + (20 * j_idx)] = 'Y';  
						  else
                           if (module_idx == 25)
                              refbuf[8 + (20 * j_idx)] = 'Z';
             
                           strcpy((char *)refbuf + 9 + (20 * j_idx), inbound_ptr->call);
                           refbuf[16 + (20 * j_idx)] = inbound_ptr->links[module_idx];

                           refbuf[17 + (20 * j_idx)] = 0;
                           refbuf[18 + (20 * j_idx)] = 0;
                           refbuf[19 + (20 * j_idx)] = 0;
                           refbuf[20 + (20 * j_idx)] = 0x50;
                           refbuf[21 + (20 * j_idx)] = 0x04;
                           refbuf[22 + (20 * j_idx)] = 0x32;
                           refbuf[23 + (20 * j_idx)] = 0x4d;
                           refbuf[24 + (20 * j_idx)] = 0x9f;
                           refbuf[25 + (20 * j_idx)] = 0xdb;
                           refbuf[26 + (20 * j_idx)] = 0x0e;
                           refbuf[27 + (20 * j_idx)] = 0;

                           j_idx++;

                           if (j_idx == 39)
                           {
                              /* 20 bytes for each user, so 39 * 20 = 780 bytes + 8 bytes header = 788 */
                              refbuf[0] = 0x14;
                              refbuf[1] = 0xc3;

                              k_idx = i_idx - 38;
                              memcpy(tmp, (char *)&k_idx, 2);
                              refbuf[4] = tmp[0];
                              refbuf[26] = tmp[1];

                              sendto(ref_sock,(char *)refbuf,788,0,
                                     (struct sockaddr *)&fromInbound,
                                     sizeof(struct sockaddr_in));

                              j_idx = 0;
                           }
                        }
                     }
                  }
               }

               if (j_idx != 0)
               {
                  k_idx = 8 + (j_idx * 20);
                  memcpy(tmp, (char *)&k_idx, 2);
                  refbuf[0] = tmp[0];
                  refbuf[1] = tmp[1] | 0xc0;

                  if (i_idx > j_idx)
                     k_idx = i_idx - j_idx;
                  else
                     k_idx = 0;

                  mempcpy(tmp, (char *)&k_idx, 2);
                  refbuf[4] = tmp[0];
                  refbuf[26] = tmp[1];

                  sendto(ref_sock,(char *)refbuf, 8 + (j_idx * 20), 0,
                         (struct sockaddr *)&fromInbound,
                         sizeof(struct sockaddr_in));
               }
            }
         }
         else
         /* connected user list request */
         if ((recvlen3 == 4) &&
             (refbuf[0] == 4) &&
             (refbuf[1] == 192) &&
             (refbuf[2] == 6) &&
             (refbuf[3] == 0))
         {
            unsigned short i_idx = 0;
            unsigned short j_idx = 0;
            unsigned short k_idx = 0;
            unsigned char tmp[2];
            unsigned short total = 0;

            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               /* header is 8 bytes */
 
               /* reply type */
               refbuf[2] = 6;
               refbuf[3] = 0;

               /* total connected users */
               total =  inbound_list.size();
               memcpy(tmp, (char *)&total, 2);
               refbuf[6] = tmp[0];
               refbuf[7] = tmp[1];

               for (inbound_pos = inbound_list.begin(), i_idx = 0; inbound_pos != inbound_list.end();  inbound_pos++, i_idx++)
               {
                  /* each entry has 20 bytes */
                  inbound_ptr = (inbound *)inbound_pos->second;
                  if (!inbound_ptr->is_ref)
                  {
                     refbuf[8 + (20 * j_idx)] = inbound_ptr->mod;
                     strcpy((char *)refbuf + 9 + (20 * j_idx), inbound_ptr->call);

                     /* 11 bytes here */
                     refbuf[17 + (20 * j_idx)] = 0;
                     /* refbuf[18 + (20 * j_idx)] = 0; */
                     if (memcmp(inbound_ptr->serial, "AP", 2) == 0)
                        refbuf[18 + (20 * j_idx)] = 'A';   /* dvap */
                     else
                     if (memcmp(inbound_ptr->serial, "DV019999", 8) == 0)
                        refbuf[18 + (20 * j_idx)] = 'H';   /* spot */
                     else
                        refbuf[18 + (20 * j_idx)] = 'D';   /* dongle */
                     refbuf[19 + (20 * j_idx)] = 0;
                     refbuf[20 + (20 * j_idx)] = 0x0d;
                     refbuf[21 + (20 * j_idx)] = 0x4d;
                     refbuf[22 + (20 * j_idx)] = 0x37;
                     refbuf[23 + (20 * j_idx)] = 0x4d;
                     refbuf[24 + (20 * j_idx)] = 0x6f;
                     refbuf[25 + (20 * j_idx)] = 0x98;
                     refbuf[26 + (20 * j_idx)] = 0x04;
                     refbuf[27 + (20 * j_idx)] = 0;

                     j_idx++;

                     if (j_idx == 39)
                     {
                        /* 20 bytes for each user, so 39 * 20 = 788 bytes + 8 bytes header = 788 */
                        refbuf[0] = 0x14; 
                        refbuf[1] = 0xc3;

                        k_idx = i_idx - 38;
                        memcpy(tmp, (char *)&k_idx, 2);
                        refbuf[4] = tmp[0];
                        refbuf[26] = tmp[1];

                        sendto(ref_sock,(char *)refbuf,788,0,
                               (struct sockaddr *)&fromInbound,
                               sizeof(struct sockaddr_in));

                        j_idx = 0;
                     }
                  }
               }

               if (j_idx != 0)
               {
                  k_idx = 8 + (j_idx * 20);
                  memcpy(tmp, (char *)&k_idx, 2);
                  refbuf[0] = tmp[0];
                  refbuf[1] = tmp[1] | 0xc0;

                  if (i_idx > j_idx)
                     k_idx = i_idx - j_idx;
                  else
                     k_idx = 0;

                  mempcpy(tmp, (char *)&k_idx, 2);
                  refbuf[4] = tmp[0];
                  refbuf[26] = tmp[1];

                  sendto(ref_sock,(char *)refbuf, 8 + (j_idx * 20), 0,
                         (struct sockaddr *)&fromInbound,
                         sizeof(struct sockaddr_in));
               }
            }
         }
         else
         /* date request */
         if ((recvlen3 == 4) &&
             (refbuf[0] == 4) &&
             (refbuf[1] == 192) &&
             (refbuf[2] == 8) &&
             (refbuf[3] == 0))
         {
            time_t ltime;
            struct tm tm;

            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               time(&ltime);
               localtime_r(&ltime,&tm);
      
               refbuf[0] = 34;
               refbuf[1] = 192;
               refbuf[2] = 8;
               refbuf[3] = 0;
               refbuf[4] = 0xb5;
               refbuf[26] = 0xae;
               refbuf[6] = 0x37;
               refbuf[7] = 0x4d;
               snprintf((char *)refbuf + 8, READBUFFER_SIZE - 1, 
                     "20%02d/%02d/%02d %02d:%02d:%02d %5.5s",
                     tm.tm_year % 100, tm.tm_mon+1,tm.tm_mday,
                     tm.tm_hour,tm.tm_min,tm.tm_sec, 
                     (tzname[0] == NULL)?"     ":tzname[0]);
                      
               sendto(ref_sock,(char *)refbuf,34,0,
                      (struct sockaddr *)&fromInbound,
                      sizeof(struct sockaddr_in));
            }
         } 
         else 
         /* version request */
         if ((recvlen3 == 4) &&
             (refbuf[0] == 4) &&
             (refbuf[1] == 192) &&
             (refbuf[2] == 3) &&
             (refbuf[3] == 0)) 
         {
            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               refbuf[0] = 9;
               refbuf[1] = 192;
               refbuf[2] = 3;
               refbuf[3] = 0;
               strncpy((char *)refbuf + 4, VERSION, 4); 
               refbuf[8] = 0;

              sendto(ref_sock,(char *)refbuf,9,0,
                     (struct sockaddr *)&fromInbound,
                     sizeof(struct sockaddr_in));
            }
         }
         else 
         /* dvap, dvtool disconnects */
         if ((recvlen3 == 5) &&
             (refbuf[0] == 5) &&
             (refbuf[1] == 0) &&
             (refbuf[2] == 24) &&
             (refbuf[3] == 0) &&
             (refbuf[4] == 0))
         {
            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               /* reply with the same DISCONNECT */
               sendto(ref_sock,(char *)refbuf,5,0,
                   (struct sockaddr *)&fromInbound,
                   sizeof(struct sockaddr_in));

               inbound_ptr = (inbound *)inbound_pos->second;
               if (memcmp(inbound_ptr->call, "1NFO", 4) != 0)
                  printf("Call %s disconnected\n", inbound_ptr->call);
               free(inbound_pos->second);
               inbound_pos->second = NULL;
               inbound_list.erase(inbound_pos);
            }
            else
              printf("Disconnect received from %s-%s\n", an_ip, a_port);
         }
         else
         if ((recvlen3 == 5) &&
             (refbuf[0] == 5) &&
             (refbuf[1] == 0) &&
             (refbuf[2] == 24) &&
             (refbuf[3] == 0) &&
             (refbuf[4] == 1))
         {
            /* If we already have this ip, that means that we already requested a link to ref */

            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               printf("Connect request accepted from %s-%s, sending login\n", an_ip, a_port);
               /* send a login */
               refbuf[0] = 28;
               refbuf[1] = 192;
               refbuf[2] = 4;
               refbuf[3] = 0;

               memcpy(refbuf + 4, ADMIN, CALL_SIZE);
               for (j = 11; j > 3; j--)
               {
                  if (refbuf[j] == ' ')
                     refbuf[j] = '\0';
                  else
                     break;
               }
               memset(refbuf + 12, '\0', 8);
               memcpy(refbuf + 20, "DV019999", 8);

               sendto(ref_sock,(char *)refbuf,28,0,
                      (struct sockaddr *)&fromInbound,
                      sizeof(fromInbound));
            }
            else   /* They want to connect with our reflector */
            {
               if ((inbound_list.size() + 1) > MAX_OTHER_USERS)
                  printf("Inbound connection from %s-%s but over the MAX_OTHER_USERS limit of %ld\n",
                          an_ip, a_port, inbound_list.size());
               else
                  sendto(ref_sock,(char *)refbuf,5,0,
                         (struct sockaddr *)&fromInbound,
                         sizeof(fromInbound));
            }
         }
         else
         if ((recvlen3 == 8) &&  /* this is a response to our login request */
             (refbuf[0] == 8) &&
             (refbuf[1] == 192) &&
             (refbuf[2] == 4) &&
             (refbuf[3] == 0))
         {
            /* we should already have the ip address in our list */
            if ((refbuf[4] == 79) &&
                (refbuf[26] == 75) &&
                (refbuf[6] == 82))
               printf("Login OK to %s\n", an_ip);
            else
               printf("Login to %s failed\n", an_ip);
         }
         else
         if ((recvlen3 == 28) &&  /* they want to login to our reflector */
             (refbuf[0] == 28) &&
             (refbuf[1] == 192) &&
             (refbuf[2] == 4) &&
             (refbuf[3] == 0))
         {
            /* verify callsign */
            memcpy(a_call, refbuf + 4, CALL_SIZE);
            a_call[CALL_SIZE] = '\0';
            for (i = 7; i > 0; i--)
            {
               if (a_call[i] == '\0')
                  a_call[i] = ' ';
               else
                  break;
            }

            /***
            if (memcmp(a_call, "1NFO", 4) != 0)
               printf("Possible new connection: %s, ip=%s, DV=%.8s\n",
                       a_call, an_ip, refbuf + 20);
            ***/

            strcpy(a_call2, a_call); a_call2[7] = ' ';
            call_valid = regexec(&preg, a_call, 0, NULL, 0);
            if ((call_valid != REG_NOERROR) ||
                (blocks.find(a_call2) != blocks.end()) ||     /* BLOCKED a_call */
                (memcmp(a_call, OWNER, OWNER_SIZE) == 0))    /* a_call can NOT be our reflector */
            {
               printf("Invalid(or blocked) CALL=%s(%s),ip=%s,%s\n", a_call, a_call2, an_ip, a_port);

               refbuf[0] = 8;
               refbuf[4] = 70;
               refbuf[26] = 65;
               refbuf[6] = 73;
               refbuf[7] = 76;

               sendto(ref_sock,(char *)refbuf,8,0,
                      (struct sockaddr *)&fromInbound,
                      sizeof(fromInbound));
            }
            else
            {
               if ((inbound_list.size() + 1) > MAX_OTHER_USERS)
               {
                  printf("Inbound connection from %s,%s but over the MAX_OTHER_USERS limit of %ld\n",
                          an_ip, a_port, inbound_list.size());

                  refbuf[0] = 8;
                  refbuf[4] = 70;
                  refbuf[26] = 65;
                  refbuf[6] = 73;
                  refbuf[7] = 76;

                  sendto(ref_sock,(char *)refbuf,8,0,
                         (struct sockaddr *)&fromInbound,
                         sizeof(fromInbound));
               }
               else
               {
                  /* add the dongle to the inbound list */
                  inbound_ptr = (inbound *)malloc(sizeof(inbound));
                  if (inbound_ptr)
                  {
                     inbound_ptr->countdown = TIMEOUT;
                     memcpy((char *)&(inbound_ptr->sin),(char *)&fromInbound, sizeof(struct sockaddr_in));
                     strcpy(inbound_ptr->call, a_call);
                     time(&(inbound_ptr->connect_time));
                     inbound_ptr->isMute = false;
                     inbound_ptr->mod = ' ';
                     inbound_ptr->is_ref = false;
                     inbound_ptr->links[0] = inbound_ptr->links[1] = inbound_ptr->links[2] = inbound_ptr->links[3] = inbound_ptr->links[4] = ' ';

                     memcpy(inbound_ptr->serial, refbuf + 20, 8); inbound_ptr->serial[8] = '\0';

                     sprintf(search_value, "%s-%s", an_ip, a_port);
                     inbound_insert_pair = inbound_list.insert(pair<string, inbound *>(search_value, inbound_ptr));
                     if (inbound_insert_pair.second)
                     {
                        if (memcmp(inbound_ptr->call, "1NFO", 4) != 0)
                           printf("new CALL=%s,DONGLE,ip=%s-%s, DV=%.8s, users=%ld\n",
                                   inbound_ptr->call,an_ip, a_port, refbuf + 20, inbound_list.size() + a_user_list.size());

                        refbuf[0] = 8;
                        refbuf[4] = 79;
                        refbuf[26] = 75;
                        refbuf[6] = 82;
                        refbuf[7] = 87;

                        sendto(ref_sock,(char *)refbuf,8,0,
                               (struct sockaddr *)&fromInbound,
                               sizeof(fromInbound));
                     }
                     else
                     {
                        printf("failed to add CALL=%s,ip=%s,%s\n",inbound_ptr->call,an_ip,a_port);
                        free(inbound_ptr);
                        inbound_ptr = NULL;

                        refbuf[0] = 8;
                        refbuf[4] = 70;
                        refbuf[26] = 65;
                        refbuf[6] = 73;
                        refbuf[7] = 76;

                        sendto(ref_sock,(char *)refbuf,8,0,
                            (struct sockaddr *)&fromInbound,
                            sizeof(fromInbound));
                     }
                  }
                  else
                  {
                     printf("malloc() failed for call=%s,ip=%s,%s\n",a_call,an_ip,a_port);

                     refbuf[0] = 8;
                     refbuf[4] = 70;
                     refbuf[26] = 65;
                     refbuf[6] = 73;
                     refbuf[7] = 76;

                     sendto(ref_sock,(char *)refbuf,8,0,
                         (struct sockaddr *)&fromInbound,
                         sizeof(fromInbound));
                  }
               }
            }
         }
         else
         if ((recvlen3 == 3) && (memcmp(refbuf, REF_ACK, 3) == 0))  /* keepalive */
         {
            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               inbound_ptr = (inbound *)inbound_pos->second;
               inbound_ptr->countdown = TIMEOUT;
            }
         }
         else
         if ( ((recvlen3 == 58) ||
               (recvlen3 == 29) ||
               (recvlen3 == 32)) &&
              (memcmp(refbuf + 2, "DSVT", 4) == 0) &&
              ((refbuf[6] == 0x10) ||
               (refbuf[6] == 0x20)) &&
              (refbuf[10] == 0x20))
         {
            found = false;

            sprintf(search_value, "%s-%s", an_ip, a_port);
            inbound_pos = inbound_list.find(search_value);
            if (inbound_pos != inbound_list.end())
            {
               inbound_ptr = (inbound *)inbound_pos->second;
               inbound_ptr->countdown = TIMEOUT;

               /* We drop audio from muted stations */
               if (!inbound_ptr->isMute)
                  found = true;

               if (recvlen3 == 58)
               {
                  memcpy(a_call, refbuf + 44, CALL_SIZE);
                  a_call[CALL_SIZE] = '\0';
                  
                  call_valid = regexec(&preg, a_call, 0, NULL, 0);
                  if (call_valid != REG_NOERROR) 
                  {
                     printf("User callsign [%s] not valid, audio from [%s] will not be transmitted\n", a_call, search_value);
                     found = false;
                  }
                  else
                  {
                     a_call[7] = ' ';
                     if (blocks.find(a_call) != blocks.end())
                     {
                        printf("User callsign [%s] blocked, audio from [%s] will not be transmitted\n", a_call, search_value);
                        found = false;
                     }
                  }
               }
            }

            if (found)
            {
               sprintf(an_rcd_streamid,"%s-%d,%d", an_ip,refbuf[14],refbuf[15]);
               if (recvlen3 == 58)
               {
                  if (QSO_DETAILS)
                  {
                     if (!inbound_ptr->is_ref)
                        source_mod = ' ';
                     else
                     {
                        if (refbuf[35] == 'G')
                           source_mod = refbuf[27];
                        else
                        if (refbuf[27] == 'G')
                           source_mod = refbuf[35];
                        else
                           source_mod = refbuf[27];
                     }
                     printf("START user: streamID=%d,%d, my=%.8s, sfx=%.4s, ur=%.8s, rpt1=%.8s, rpt2=%.8s, %d bytes fromIP=%s, src=%.8s %c\n",
                           refbuf[14],refbuf[15],&refbuf[44],
                           &refbuf[52], &refbuf[36],
                           &refbuf[20], &refbuf[28],
                           recvlen3, an_ip,
                           inbound_ptr->call, source_mod);
                  } 

                  /* the crazy shit */

                  /* replace the rpt1 name and rpt2 name with our callsign */
                  /* leave the module of the remote system untouched */

                  if (refbuf[35] == 'G')  /* dongle,  */
                  {
                     memcpy(refbuf + 20, OWNER, 7);
                     memcpy(refbuf + 28, OWNER, 7); 
                  }
                  else
                  if (refbuf[27] == 'G') /* RF user thru local repeater ---> reflector */
                  {
                     refbuf[27] = refbuf[35];
                     refbuf[35] = 'G';
                     memcpy(refbuf + 20, OWNER, 7);
                     memcpy(refbuf + 28, OWNER, 7);
                  }
                  else /* dvap */
                  {
                     memcpy(refbuf + 20, OWNER, 7);
                     memcpy(refbuf + 28, OWNER, 7);
                     refbuf[35] = 'G';
                  }
               }
               else
               {
                  if ((refbuf[16] & 0x40) != 0)
                  {
                     if (QSO_DETAILS)
                        printf("END user: streamID=%d,%d, %d bytes from IP=%s\n",
                                 refbuf[14],refbuf[15],recvlen3, an_ip);
                  }

               }

               /* send the data to repeaters */
               if (recvlen3 == 58)
               {
                  source_mod = ' ';

                  if (inbound_ptr->is_ref)
                  {
                     for (i = 0; i < 26; i++)
                     {
                        /* if the remote module matches */
                        if ((inbound_ptr->links[i] == refbuf[27]) &&
                            (refbuf[27] != ' '))
                        {
                           if (i == 0)
                              refbuf[27] = 'A';
                           else
                           if (i == 1)
                              refbuf[27] = 'B';
                           else
                           if (i == 2)
                              refbuf[27] = 'C';
                           else
                           if (i == 3)
                              refbuf[27] = 'D';
						  else
                           if (i == 4)
                              refbuf[27] = 'E';
						  else
                           if (i == 5)
                              refbuf[27] = 'F';
						  else
                           if (i == 6)
                              refbuf[27] = 'G';
						  else
                           if (i == 7)
                              refbuf[27] = 'H';
						  else
                           if (i == 8)
                              refbuf[27] = 'I';
						  else
                           if (i == 9)
                              refbuf[27] = 'J';
						  else
                           if (i == 10)
                              refbuf[27] = 'K';
						  else
                           if (i == 11)
                              refbuf[27] = 'L';
						  else
                           if (i == 12)
                              refbuf[27] = 'M';
						  else
                           if (i == 13)
                              refbuf[27] = 'N';
						  else
                           if (i == 14)
                              refbuf[27] = 'O';
						  else
                           if (i == 15)
                              refbuf[27] = 'P';
						  else
                           if (i == 16)
                              refbuf[27] = 'Q';
						  else
                           if (i == 17)
                              refbuf[27] = 'R';
						  else
                           if (i == 18)
                              refbuf[27] = 'S';
						  else
                           if (i == 19)
                              refbuf[27] = 'T';
						  else
                           if (i == 20)
                              refbuf[27] = 'U';
						  else
                           if (i == 21)
                              refbuf[27] = 'V';
						  else
                           if (i == 22)
                              refbuf[27] = 'W';
						  else
                           if (i == 23)
                              refbuf[27] = 'X';
						  else
                           if (i == 24
                              refbuf[27] = 'Y;
                           else
                              refbuf[27] = 'Z';

                           source_mod = refbuf[27];

                           if (refbuf[27] == 'E')
                           {
                              rcd_pos = rcd_list.find(an_rcd_streamid);
                              if (rcd_pos == rcd_list.end())
                              {
                                 if (rcd_list.size() < MAX_RCD_USERS)
                                 {
                                    an_rcd = (struct rcd *)malloc(sizeof(struct rcd));
                                    if (an_rcd)
                                    {
                                       an_rcd->locked = false;
                                       time(&(an_rcd->ts)); /* stamp it */
                                       memcpy((char *)&(an_rcd->sin), (char *)&fromInbound, sizeof(struct sockaddr_in));
                                       memcpy(an_rcd->data[0], refbuf, recvlen3);
                                       an_rcd->recvlen = recvlen3;
                                       an_rcd->idx = 1;
                                       rcd_insert_pair = rcd_list.insert(pair<string, struct rcd *>(an_rcd_streamid, an_rcd));
                                       if (rcd_insert_pair.second)
                                          ; // printf("Started recording for user %.8s, streamid=%d,%d\n",
                                            //          refbuf + 44, refbuf[14],refbuf[15]);
                                       else
                                       {
                                          printf("failed to add user %.8s, streamid=%d,%d, for recording...\n",
                                                  refbuf + 44, refbuf[14],refbuf[15]);
                                          free(an_rcd);
                                          an_rcd = NULL;
                                       }
                                    }
                                    else
                                       printf("Failed to allocate for recording on user %.8s, streamid=%d,%d\n",
                                               refbuf + 44, refbuf[14],refbuf[15]);
                                 }
                                 else
                                    printf("Reached maximum concurrent users for recording\n");
                              }
                           }

                           /* This user is talking on this module */
                           inbound_ptr->mod = refbuf[27];

                           // put user into tmp1
                           memcpy(tmp1, refbuf + 44, 8); tmp1[8] = '\0';

                           // delete the user if exists
                           for (dt_lh_pos = dt_lh_list.begin(); dt_lh_pos != dt_lh_list.end();  dt_lh_pos++)
                           {
                              if (strcmp((char *)dt_lh_pos->second.c_str(), tmp1) == 0)
                              {
                                 dt_lh_list.erase(dt_lh_pos);
                                 break;
                              }
                           }
                           /* Limit?, delete oldest user */
                           if (dt_lh_list.size() == LH_MAX_SIZE)
                           {
                              dt_lh_pos = dt_lh_list.begin();
                              dt_lh_list.erase(dt_lh_pos); 
                           }
                           /* add user */
                           time(&tNow);
                           sprintf(tmp2, "%ld=%.7s%c", tNow, inbound_ptr->call, refbuf[27]);
                           dt_lh_list[tmp2] = tmp1;

                           if (rcd_list.find(an_rcd_streamid) == rcd_list.end())
                           {
/*************** REPLACEMENT to send_a_pkt_to_all **************************************/
                              /* send header to repeaters */
                              if (refbuf[27] == 'A')
                                 k = 0;
                              else
                              if (refbuf[27] == 'B')
                                 k = 1;
                              else
                              if (refbuf[27] == 'C')
                                 k = 2;
                              else
                              if (refbuf[27] == 'D')
                                 k = 3;
							 else
                              if (refbuf[27] == 'E)
                                 k = 4;
							 else
                              if (refbuf[27] == 'F')
                                 k = 5;
							 else
                              if (refbuf[27] == 'G')
                                 k = 6;
							 else
                              if (refbuf[27] == 'H')
                                 k = 7;
							 else
                              if (refbuf[27] == 'I')
                                 k = 8;
							 else
                              if (refbuf[27] == 'J')
                                 k = 9;
							 else
                              if (refbuf[27] == 'K')
                                 k = 10;
							 else
                              if (refbuf[27] == 'L')
                                 k = 11;
							 else
                              if (refbuf[27] == 'M')
                                 k = 12;
							 else
                              if (refbuf[27] == 'N')
                                 k = 13;
							 else
                              if (refbuf[27] == 'O')
                                 k = 14;
							 else
                              if (refbuf[27] == 'P')
                                 k = 15;
							 else
                              if (refbuf[27] == 'Q')
                                 k = 16;
							 else
                              if (refbuf[27] == 'R')
                                 k = 17;
							 else
                              if (refbuf[27] == 'S')
                                 k = 18;
							 else
                              if (refbuf[27] == 'T')
                                 k = 19;
							 else
                              if (refbuf[27] == 'U')
                                 k = 20;
							 else
                              if (refbuf[27] == 'V')
                                 k = 21;
							 else
                              if (refbuf[27] == 'W')
                                 k = 22;
							 else
                              if (refbuf[27] == 'X')
                                 k = 23;
							 else
                              if (refbuf[27] == 'Y')
                                 k = 24;
							 else
                              if (refbuf[27] == 'Z')
                                 k = 25;
                              else
                                 k = -1;

                              if (k >= 0)
                              {
                                 temp_x[k].old_sid[0] = refbuf[14];   temp_x[k].old_sid[1] = refbuf[15];

                                 streamid_raw = (refbuf[14] * 256U) + refbuf[15];
                                 streamid_raw ++;
                                 if (streamid_raw == 0)
                                    streamid_raw ++;

                                 memcpy(temp_x[k].hdr, refbuf + 2, 56);
                                 temp_x[k].hdr[12] = streamid_raw / 256U;
                                 temp_x[k].hdr[13] = streamid_raw % 256U;

                                 temp_x[k].s_addr = fromInbound.sin_addr.s_addr;

                                 for (user_pos = a_user_list.begin(); user_pos != a_user_list.end(); user_pos++)
                                 {
                                    a_user_ptr = (struct a_user *)user_pos->second;

                                    if ((a_user_ptr->rpt_mods[k][0] != '\0') ||
                                        (a_user_ptr->rpt_mods[k][1] != '\0') ||
                                        (a_user_ptr->rpt_mods[k][2] != '\0') ||
                                        (a_user_ptr->rpt_mods[k][3] != '\0'))
                                       sendto(srv_sock, (char *)temp_x[k].hdr, 56,
                                              0,(struct sockaddr *)&(a_user_ptr->sin),
                                              sizeof(struct sockaddr_in));
                                 }
                              }
/*********************************************************************************************/
                           }
                           break;
                        }
                     }
                  }                     
                  else
                  if (refbuf[27] != ' ')
                  {
                     source_mod = refbuf[27];

                     if (refbuf[27] == 'E')
                     {
                        rcd_pos = rcd_list.find(an_rcd_streamid);
                        if (rcd_pos == rcd_list.end())
                        {
                           if (rcd_list.size() < MAX_RCD_USERS)
                           {
                              an_rcd = (struct rcd *)malloc(sizeof(struct rcd));
                              if (an_rcd)
                              {
                                 an_rcd->locked = false;
                                 time(&(an_rcd->ts)); /* stamp it */
                                 memcpy((char *)&(an_rcd->sin), (char *)&fromInbound, sizeof(struct sockaddr_in));
                                 memcpy(an_rcd->data[0], refbuf, recvlen3);
                                 an_rcd->recvlen = recvlen3;
                                 an_rcd->idx = 1;
                                 rcd_insert_pair = rcd_list.insert(pair<string, struct rcd *>(an_rcd_streamid, an_rcd));
                                 if (rcd_insert_pair.second)
                                    ; // printf("Started recording for user %.8s, streamid=%d,%d\n",
                                      //         refbuf + 44, refbuf[14],refbuf[15]);
                                 else
                                 {
                                    printf("failed to add user %.8s, streamid=%d,%d, for recording...\n",
                                            refbuf + 44, refbuf[14],refbuf[15]);
                                    free(an_rcd);
                                    an_rcd = NULL;
                                 }
                              }
                              else
                                 printf("Failed to allocate for recording on user %.8s, streamid=%d,%d\n",
                                         refbuf + 44, refbuf[14],refbuf[15]);
                           }
                           else
                              printf("Reached maximum concurrent users for recording\n");
                        }
                     }

                     /* This user is talking on this module */
                     inbound_ptr->mod = refbuf[27];

                     // put user into tmp1
                     memcpy(tmp1, refbuf + 44, 8); tmp1[8] = '\0';

                     // delete the user if exists
                     for (dt_lh_pos = dt_lh_list.begin(); dt_lh_pos != dt_lh_list.end();  dt_lh_pos++)
                     {
                        if (strcmp((char *)dt_lh_pos->second.c_str(), tmp1) == 0)
                        {
                           dt_lh_list.erase(dt_lh_pos);
                           break;
                        }
                     }
                     /* Limit?, delete oldest user */
                     if (dt_lh_list.size() == LH_MAX_SIZE)
                     {
                        dt_lh_pos = dt_lh_list.begin();
                        dt_lh_list.erase(dt_lh_pos);
                     }
                     time(&tNow);
                     sprintf(tmp2, "%ld=%.7s%c", tNow, inbound_ptr->call, refbuf[27]);
                     dt_lh_list[tmp2] = tmp1;

                     if (rcd_list.find(an_rcd_streamid) == rcd_list.end())
                     {
/********************** REPLACEMENT to send_a_pkt_to_all ***********************************************/
                        /* send header to repeaters */
					   if (refbuf[27] == 'A')
							 k = 0;
						  else
						  if (refbuf[27] == 'B')
							 k = 1;
						  else
						  if (refbuf[27] == 'C')
							 k = 2;
						  else
						  if (refbuf[27] == 'D')
							 k = 3;
						 else
						  if (refbuf[27] == 'E)
							 k = 4;
						 else
						  if (refbuf[27] == 'F')
							 k = 5;
						 else
						  if (refbuf[27] == 'G')
							 k = 6;
						 else
						  if (refbuf[27] == 'H')
							 k = 7;
						 else
						  if (refbuf[27] == 'I')
							 k = 8;
						 else
						  if (refbuf[27] == 'J')
							 k = 9;
						 else
						  if (refbuf[27] == 'K')
							 k = 10;
						 else
						  if (refbuf[27] == 'L')
							 k = 11;
						 else
						  if (refbuf[27] == 'M')
							 k = 12;
						 else
						  if (refbuf[27] == 'N')
							 k = 13;
						 else
						  if (refbuf[27] == 'O')
							 k = 14;
						 else
						  if (refbuf[27] == 'P')
							 k = 15;
						 else
						  if (refbuf[27] == 'Q')
							 k = 16;
						 else
						  if (refbuf[27] == 'R')
							 k = 17;
						 else
						  if (refbuf[27] == 'S')
							 k = 18;
						 else
						  if (refbuf[27] == 'T')
							 k = 19;
						 else
						  if (refbuf[27] == 'U')
							 k = 20;
						 else
						  if (refbuf[27] == 'V')
							 k = 21;
						 else
						  if (refbuf[27] == 'W')
							 k = 22;
						 else
						  if (refbuf[27] == 'X')
							 k = 23;
						 else
						  if (refbuf[27] == 'Y')
							 k = 24;
						 else
						  if (refbuf[27] == 'Z')
							 k = 25;
						  else
							 k = -1;

                        if (k >= 0)
                        {
                           temp_x[k].old_sid[0] = refbuf[14];   temp_x[k].old_sid[1] = refbuf[15];

                           streamid_raw = (refbuf[14] * 256U) + refbuf[15];
                           streamid_raw ++;
                           if (streamid_raw == 0)
                              streamid_raw ++;

                           memcpy(temp_x[k].hdr, refbuf + 2, 56);
                           temp_x[k].hdr[12] = streamid_raw / 256U;
                           temp_x[k].hdr[13] = streamid_raw % 256U;

                           temp_x[k].s_addr = fromInbound.sin_addr.s_addr;

                           for (user_pos = a_user_list.begin(); user_pos != a_user_list.end(); user_pos++)
                           {
                              a_user_ptr = (struct a_user *)user_pos->second;

                              if ((a_user_ptr->rpt_mods[k][0] != '\0') ||
                                  (a_user_ptr->rpt_mods[k][1] != '\0') ||
                                  (a_user_ptr->rpt_mods[k][2] != '\0') ||
                                  (a_user_ptr->rpt_mods[k][3] != '\0'))
                                 sendto(srv_sock, (char *)temp_x[k].hdr, 56,
                                        0,(struct sockaddr *)&(a_user_ptr->sin),
                                        sizeof(struct sockaddr_in));
                           }
                        }
/**********************************************************************************************************/
                     }
                  }
               }
               else
               {
                  rcd_pos = rcd_list.find(an_rcd_streamid);
                  if (rcd_pos != rcd_list.end())
                  {
                     an_rcd = (struct rcd *)rcd_pos->second;
                     time(&(an_rcd->ts)); /* stamp it */
                     if ((an_rcd->idx < MAX_RCD_DATA) && !(an_rcd->locked))
                     {
                        memcpy(an_rcd->data[an_rcd->idx], refbuf, recvlen3);
                        an_rcd->idx ++;
                     }
                     if (((refbuf[16] & 0x40) != 0) && !(an_rcd->locked))
                     {
                        /***
                        printf("Finished recording for user %.8s, streamid=%d,%d\n",
                                 &(an_rcd->data[0][44]), refbuf[14], refbuf[15]);
                        ***/

                        /*** start playback */
                        an_rcd->locked = true;
                        pthread_attr_init(&attr);
                        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                        rc = pthread_create(&playback_thread,&attr,playback,(void *)rcd_pos->second);
                        if (rc != 0)
                        {
                           printf("Failed to start the playback thread for streamid=%d,%d\n",
                                    refbuf[14], refbuf[15]);
                           free(rcd_pos->second);
                           rcd_pos->second = NULL;
                           rcd_list.erase(rcd_pos);
                        }
                        pthread_attr_destroy(&attr);
                     }
                  }
                  else
                  {
/******************************* REPLACEMENT to send_a_pkt_to_all **********************************/
                     for (user_pos = a_user_list.begin(); user_pos != a_user_list.end(); user_pos++)
                     {
                        a_user_ptr = (struct a_user *)user_pos->second;

                        /* source must NOT be a reflector */
                        if (!(inbound_ptr->is_ref))
                        {
                           if ((refbuf[26] == 0x55) && (refbuf[27] == 0x2d) && (refbuf[28] == 0x16))
                           {
                              for (k = 0; k < 4; k++)
                              {
                                 if ((refbuf[14] == temp_x[k].old_sid[0]) &&
                                     (refbuf[15] == temp_x[k].old_sid[1]) &&
                                     (fromInbound.sin_addr.s_addr == temp_x[k].s_addr) &&
                                     ((a_user_ptr->rpt_mods[k][0] != '\0') || /* module is linked */
                                      (a_user_ptr->rpt_mods[k][1] != '\0') ||
                                      (a_user_ptr->rpt_mods[k][2] != '\0') ||
                                      (a_user_ptr->rpt_mods[k][3] != '\0')))
                                 {
                                    sendto(srv_sock, (char *)temp_x[k].hdr, 56, 0,
                                           (struct sockaddr *)&(a_user_ptr->sin), sizeof(struct sockaddr_in));
                                    break;
                                 }
                              }
                           }
                        }

                        for (k = 0; k < 4; k++)
                        {
                           if ((refbuf[14] == temp_x[k].old_sid[0]) &&
                               (refbuf[15] == temp_x[k].old_sid[1]) &&
                               (fromInbound.sin_addr.s_addr == temp_x[k].s_addr) &&
                               ((a_user_ptr->rpt_mods[k][0] != '\0') || /* module is linked */
                                (a_user_ptr->rpt_mods[k][1] != '\0') ||
                                (a_user_ptr->rpt_mods[k][2] != '\0') ||
                                (a_user_ptr->rpt_mods[k][3] != '\0')))
                           {
                              refbuf[14] = temp_x[k].hdr[12];  refbuf[15] = temp_x[k].hdr[13];
                              sendto(srv_sock, (char *)(refbuf + 2), 27,
                                     0,(struct sockaddr *)&(a_user_ptr->sin),
                                     sizeof(struct sockaddr_in));
                              refbuf[14] = temp_x[k].old_sid[0]; refbuf[15] = temp_x[k].old_sid[1];
                              break;
                           }
                        }
                     }
/***************************************************************************************************/
                  }
               }

               /* At this point, if this is a header, it contains our reflector info */
               /* save the header for re-transmit */
               if (recvlen3 == 58)
               {
                  i = -1;
                  if (source_mod == 'A')
                     i = 0;
                  else
                  if (source_mod == 'B')
                     i = 1;
                  else
                  if (source_mod == 'C')
                     i = 2;
                  else
                  if (source_mod == 'D')
                     i = 3;
				 else
                  if (source_mod == 'E')
                     i = 4;
				 else
                  if (source_mod == 'F')
                     i = 5;
				 else
                  if (source_mod == 'G')
                     i = 6;
				 else
                  if (source_mod == 'H')
                     i = 7;
				 else
                  if (source_mod == 'I')
                     i = 8;
				 else
                  if (source_mod == 'J')
                     i = 9;
				 else
                  if (source_mod == 'K')
                     i = 10;
				 else
                  if (source_mod == 'L')
                     i = 11;
				 else
                  if (source_mod == 'M')
                     i = 12;
				 else
                  if (source_mod == 'N')
                     i = 13;
				 else
                  if (source_mod == 'O')
                     i = 14;
				 else
                  if (source_mod == 'P')
                     i = 15;
				 else
                  if (source_mod == 'Q')
                     i = 16;
				 else
                  if (source_mod == 'R')
                     i = 17;
				 else
                  if (source_mod == 'S')
                     i = 18;
				 else
                  if (source_mod == 'T')
                     i = 19;
				 else
                  if (source_mod == 'U')
                     i = 20;
				 else
                  if (source_mod == 'V')
                     i = 21;
				 else
                  if (source_mod == 'W')
                     i = 22;
				 else
                  if (source_mod == 'X')
                     i = 23;
				 else
                  if (source_mod == 'Y')
                     i = 24;
				 else
                  if (source_mod == 'Z')
                     i = 25;

                  if (i >= 0)
                  {
                     temp_r[i].s_addr = fromInbound.sin_addr.s_addr;
                     memcpy(temp_r[i].hdr, refbuf, 58);
                  }
               }

               /* send the data to dvap, dvtool stations */
               if (rcd_list.find(an_rcd_streamid) == rcd_list.end())
               {
                  for (inbound_pos2 = inbound_list.begin(); inbound_pos2 != inbound_list.end(); inbound_pos2++)
                  {
                     if (strcmp(inbound_pos2->first.c_str(), inbound_pos->first.c_str()) != 0)
                     {
                        inbound_ptr2 = (inbound *)inbound_pos2->second;
                        if (recvlen3 == 58)
                        {
                           /* if it is a reflector, we must send its own call */
                           if (inbound_ptr2->is_ref)
                           {
                              if (i >= 0)
                              {
                                 if (inbound_ptr2->links[i] != ' ')
                                 {
                                    memcpy(refbuf + 20, inbound_ptr2->call, 7);
                                    refbuf[27] = inbound_ptr2->links[i];
                                    memcpy(refbuf + 28, inbound_ptr2->call, 7);

                                    sendto(ref_sock, (char *)refbuf, recvlen3, 0,
                                        (struct sockaddr *)&(inbound_ptr2->sin),
                                        sizeof(struct sockaddr_in));
                                 }
                              }
                           }
                           else
                           {
                              if (i >= 0)
                              {
                                 memcpy(refbuf + 20, OWNER, 7);
                                 refbuf[27] = source_mod;
                                 memcpy(refbuf + 28, OWNER, 7);

                                 sendto(ref_sock, (char *)refbuf, recvlen3, 0,
                                     (struct sockaddr *)&(inbound_ptr2->sin),
                                     sizeof(struct sockaddr_in));
                              }
                           }
                        }
                        else
                        {
                           if ((refbuf[26] == 0x55) && (refbuf[27] == 0x2d) && (refbuf[28] == 0x16))
                           {
                              if ((!inbound_ptr2->is_ref) && (!inbound_ptr->is_ref))
                              {
                                 for (i = 0; i < 4; i++)
                                 {
                                    if ((refbuf[14] == temp_r[i].hdr[14]) &&
                                        (refbuf[15] == temp_r[i].hdr[15]) &&
                                        (fromInbound.sin_addr.s_addr == temp_r[i].s_addr))
                                    {
                                       sendto(ref_sock, (char *)temp_r[i].hdr, 58, 0, 
                                           (struct sockaddr *)&(inbound_ptr2->sin), sizeof(struct sockaddr_in));
                                       break;
                                    }
                                 }
                              }
                           }

                           /* if it is a regular dongle, send it */
                           if (!inbound_ptr2->is_ref)
                              sendto(ref_sock, (char *)refbuf, recvlen3, 0, 
                                  (struct sockaddr *)&(inbound_ptr2->sin),
                                  sizeof(struct sockaddr_in));
                           else
                           {
                              /* treat the remote reflector with resepct */
                              /* send it only if the module matches */
                              for (i = 0; i < 4; i++)
                              {
                                 if ((refbuf[14] == temp_r[i].hdr[14]) &&
                                     (refbuf[15] == temp_r[i].hdr[15]) &&
                                     (fromInbound.sin_addr.s_addr == temp_r[i].s_addr) &&
                                     (inbound_ptr2->links[i] != ' '))
                                 { 
                                    sendto(ref_sock, (char *)refbuf, recvlen3, 0,
                                        (struct sockaddr *)&(inbound_ptr2->sin),
                                        sizeof(struct sockaddr_in));

                                    break;
                                 } 
                              }
                           }
                        }
                     }
                  }
               } 
            }
         }
         FD_CLR (ref_sock,&fdset);
      }

      /* process input from repeaters */
      if (FD_ISSET(srv_sock, &fdset))
      {
         fromlen = sizeof(struct sockaddr_in);
         recvlen = recvfrom(srv_sock,(char *)readBuffer,READBUFFER_SIZE,
                            0,(struct sockaddr *)&fromUser,&fromlen);

         strncpy(an_ip, inet_ntoa(fromUser.sin_addr),IP_SIZE);
         an_ip[IP_SIZE] = '\0';
         a_user_ptr = NULL;

         if ((recvlen == (CALL_SIZE + 1)) || (recvlen == (CALL_SIZE + 3)))
         {
            readBuffer[(recvlen - 1)] = '\0';

            strncpy(a_call, (char *)readBuffer,CALL_SIZE);
            a_call[CALL_SIZE] = '\0';

            /* is this a new user? */
            user_pos = a_user_list.find(an_ip);
            if (user_pos != a_user_list.end())
            {
               /* We found it */

               /* point to the data of the connected user */
               a_user_ptr = (struct a_user *)user_pos->second;

               /* update its countdown */
               a_user_ptr->countdown = TIMEOUT;

               /*
                  repeater request, an un-link request arrived ? 
                  Example:  DB0SAT space space B space NULL

                 This means that repeater DB0SAT  wants to unlink its module B from reflector
               */

               if (recvlen == (CALL_SIZE + 3)) 
               {
                  // printf("Possible new link/unlink request: %.*s\n", recvlen - 1, readBuffer);

                  /* remote repeater band */
                  if ((readBuffer[8] == 'A') || (readBuffer[8] == 'B') || (readBuffer[8] == 'C') ||
					  (readBuffer[8] == 'D') || (readBuffer[8] == 'E') || (readBuffer[8] == 'F') ||
					  (readBuffer[8] == 'G') || (readBuffer[8] == 'H') || (readBuffer[8] == 'I') ||
					  (readBuffer[8] == 'J') || (readBuffer[8] == 'K') || (readBuffer[8] == 'L') ||
					  (readBuffer[8] == 'M') || (readBuffer[8] == 'N') || (readBuffer[8] == 'O') ||
					  (readBuffer[8] == 'P') || (readBuffer[8] == 'Q') || (readBuffer[8] == 'R') ||
					  (readBuffer[8] == 'S') || (readBuffer[8] == 'T') || (readBuffer[8] == 'U') ||
					  (readBuffer[8] == 'V') || (readBuffer[8] == 'W') || (readBuffer[8] == 'X') ||
                      (readBuffer[8] == 'Y') || (readBuffer[8] == 'Z'))
                  {
                     /* local reflector module */
                     if (readBuffer[9] == ' ')
                     {
                        printf("Call %s mod %c drops the link\n",
                                 a_call, readBuffer[8]);

                        /* this is the the remote repeater band */
                        if (readBuffer[8] == 'A')
                           k = 0;
                        else
                        if (readBuffer[8] == 'B')
                           k = 1;
                        else
                        if (readBuffer[8] == 'C')
                           k = 2;
					   else
                        if (readBuffer[8] == 'D')
                           k = 3;
					   else
                        if (readBuffer[8] == 'E')
                           k = 4;
					   else
                        if (readBuffer[8] == 'F')
                           k = 5;
					   else
                        if (readBuffer[8] == 'G')
                           k = 6;
					   else
                        if (readBuffer[8] == 'H')
                           k = 7;
					   else
                        if (readBuffer[8] == 'I')
                           k = 8;
					   else
                        if (readBuffer[8] == 'J')
                           k = 9;
					   else
                        if (readBuffer[8] == 'K')
                           k = 10;
					   else
                        if (readBuffer[8] == 'L')
                           k = 11;
					   else
                        if (readBuffer[8] == 'M')
                           k = 12;
					   else
                        if (readBuffer[8] == 'N')
                           k = 13;
					   else
                        if (readBuffer[8] == 'O')
                           k = 14;
					   else
                        if (readBuffer[8] == 'P')
                           k = 15;
					   else
                        if (readBuffer[8] == 'Q')
                           k = 16;
					   else
                        if (readBuffer[8] == 'R')
                           k = 17;
					   else
                        if (readBuffer[8] == 'S')
                           k = 18;
					   else
                        if (readBuffer[8] == 'T')
                           k = 19;
					   else
                        if (readBuffer[8] == 'U')
                           k = 20;
					   else
                        if (readBuffer[8] == 'V')
                           k = 21;
					   else
                        if (readBuffer[8] == 'W')
                           k = 22;
					   else
                        if (readBuffer[8] == 'X')
                           k = 23;
					   else
                        if (readBuffer[8] == 'Y')
                           k = 24;
                        else
                           k = 25;

                        /* for all the reflector modules, zero out the repeater band */
                        for (i = 0; i < 26; i++)
                        {
                           /*** inform the remote XRF reflector: start ***/
                           if (a_user_ptr->is_xrf)
                           {
                              if (a_user_ptr->rpt_mods[i][k] != '\0')
                              {
                                 memcpy(reply_to_xrf, OWNER, 8);
                                 if (i == 0)
                                    reply_to_xrf[8] = 'A';
                                 else
                                 if (i == 1)
                                    reply_to_xrf[8] = 'B';
                                 else
                                 if (i == 2)
                                    reply_to_xrf[8] = 'C';
                                 else
                                 if (i == 3)
                                    reply_to_xrf[8] = 'D';
								 else
                                 if (i == 4)
                                    reply_to_xrf[8] = 'E';
								 else
                                 if (i == 5)
                                    reply_to_xrf[8] = 'F';
								 else
                                 if (i == 6)
                                    reply_to_xrf[8] = 'G';
								 else
                                 if (i == 7)
                                    reply_to_xrf[8] = 'H';
								 else
                                 if (i == 8)
                                    reply_to_xrf[8] = 'I';
								 else
                                 if (i == 9)
                                    reply_to_xrf[8] = 'J';
								 else
                                 if (i == 10)
                                    reply_to_xrf[8] = 'K';
								 else
                                 if (i == 11)
                                    reply_to_xrf[8] = 'L';
								 else
                                 if (i == 12)
                                    reply_to_xrf[8] = 'M';
								 else
                                 if (i == 13)
                                    reply_to_xrf[8] = 'N';
								 else
                                 if (i == 14)
                                    reply_to_xrf[8] = 'O';
								 else
                                 if (i == 15)
                                    reply_to_xrf[8] = 'P';
								 else
                                 if (i == 16)
                                    reply_to_xrf[8] = 'Q';
								 else
                                 if (i == 17)
                                    reply_to_xrf[8] = 'R';
								 else
                                 if (i == 18)
                                    reply_to_xrf[8] = 'S';
								 else
                                 if (i == 19)
                                    reply_to_xrf[8] = 'T';
								 else
                                 if (i == 20)
                                    reply_to_xrf[8] = 'U';
								 else
                                 if (i == 21)
                                    reply_to_xrf[8] = 'V';
								 else
                                 if (i == 22)
                                    reply_to_xrf[8] = 'W';
								 else
                                 if (i == 23)
                                    reply_to_xrf[8] = 'X';
								 else
                                 if (i == 24)
                                    reply_to_xrf[8] = 'Y';
                                 else
                                    reply_to_xrf[8] = 'Z';

                                 reply_to_xrf[9] = ' ';
                                 reply_to_xrf[10] = '\0';

                                 for (j = 0; j < 26; j++)
                                    sendto(srv_sock, reply_to_xrf, CALL_SIZE + 3,
                                           0,(struct sockaddr *)&(a_user_ptr->sin),
                                           sizeof(struct sockaddr_in));
                              }
                           }
                           /*** inform the remote XRF reflector: end ***/

                           a_user_ptr->rpt_mods[i][k] = '\0';
                           a_user_ptr->link_time[i][k] = 0;
                        }

                        print_links_file();

                        /* if this repeater has unlinked everything, disconnect it */
                        for (i = 0; i < 26; i++)
                        {
                           for (k = 0; k < 25; k++)
                           {
                              if (a_user_ptr->rpt_mods[i][k] != '\0')
                                 break;
                           }
                           if (k < 4)
                              break;
                        }
                        if (i == 5)
                        {
                           /* 
                              All linked modules have been unlinked,
                              disconnect the repeater
                           */
                           printf("All linked modules from %s have been unlinked, disconnecting...\n",
                                    a_call);
                           free(user_pos->second);
                           a_user_ptr = user_pos->second = NULL;
                           a_user_list.erase(user_pos);
                           // deleted = true;
                           printf("User %s ip=%s  removed\n", a_call, an_ip);
                        }
                     }
                     else
                     {
                        /*
                           A link request from remote repeater
                           Example:  DB0SAT space space BC NULL

                          This means that repeater DB0SAT  wants to link its module B
                               to our reflector module C
                        */

                        /* local reflector module */
                        if ((readBuffer[9] == 'A') || (readBuffer[9] == 'B') || (readBuffer[9] == 'C') ||
                            (readBuffer[9] == 'D') || (readBuffer[9] == 'E') || (readBuffer[9] == 'F') ||
							(readBuffer[9] == 'G') || (readBuffer[9] == 'H') || (readBuffer[9] == 'I') ||
							(readBuffer[9] == 'J') || (readBuffer[9] == 'K') || (readBuffer[9] == 'L') ||
							(readBuffer[9] == 'M') || (readBuffer[9] == 'N') || (readBuffer[9] == 'O') ||
							(readBuffer[9] == 'P') || (readBuffer[9] == 'Q') || (readBuffer[9] == 'R') ||
							(readBuffer[9] == 'S') || (readBuffer[9] == 'T') || (readBuffer[9] == 'U') ||
							(readBuffer[9] == 'V') || (readBuffer[9] == 'W') || (readBuffer[9] == 'X') ||
							(readBuffer[9] == 'Y') || (readBuffer[9] == 'Z'))
                        {

                           /* local reflector module */
							if (readBuffer[9] == 'A')
							   i = 0;
							else
							if (readBuffer[9] == 'B')
							   i = 1;
							else
							if (readBuffer[9] == 'C')
							   i = 2;
						   else
							if (readBuffer[9] == 'D')
							   i = 3;
						   else
							if (readBuffer[9] == 'E')
							   i = 4;
						   else
							if (readBuffer[9] == 'F')
							   i = 5;
						   else
							if (readBuffer[9] == 'G')
							   i = 6;
						   else
							if (readBuffer[9] == 'H')
							   i = 7;
						   else
							if (readBuffer[9] == 'I')
							   i = 8;
						   else
							if (readBuffer[9] == 'J')
							   i = 9;
						   else
							if (readBuffer[9] == 'K')
							   i = 10;
						   else
							if (readBuffer[9] == 'L')
							   i = 11;
						   else
							if (readBuffer[9] == 'M')
							   i = 12;
						   else
							if (readBuffer[9] == 'N')
							   i = 13;
						   else
							if (readBuffer[9] == 'O')
							   i = 14;
						   else
							if (readBuffer[9] == 'P')
							   i = 15;
						   else
							if (readBuffer[9] == 'Q')
							   i = 16;
						   else
							if (readBuffer[9] == 'R')
							   i = 17;
						   else
							if (readBuffer[9] == 'S')
							   i = 18;
						   else
							if (readBuffer[9] == 'T')
							   i = 19;
						   else
							if (readBuffer[9] == 'U')
							   i = 20;
						   else
							if (readBuffer[9] == 'V')
							   i = 21;
						   else
							if (readBuffer[9] == 'W')
							   i = 22;
						   else
							if (readBuffer[9] == 'X')
							   i = 23;
						   else
							if (readBuffer[9] == 'Y')
							   i = 24;
							else
							   i = 25;

                           /* remote repeater band */
							if (readBuffer[8] == 'A')
							   k = 0;
							else
							if (readBuffer[8] == 'B')
							   k = 1;
							else
							if (readBuffer[8] == 'C')
							   k = 2;
						   else
							if (readBuffer[8] == 'D')
							   k = 3;
						   else
							if (readBuffer[8] == 'E')
							   k = 4;
						   else
							if (readBuffer[8] == 'F')
							   k = 5;
						   else
							if (readBuffer[8] == 'G')
							   k = 6;
						   else
							if (readBuffer[8] == 'H')
							   k = 7;
						   else
							if (readBuffer[8] == 'I')
							   k = 8;
						   else
							if (readBuffer[8] == 'J')
							   k = 9;
						   else
							if (readBuffer[8] == 'K')
							   k = 10;
						   else
							if (readBuffer[8] == 'L')
							   k = 11;
						   else
							if (readBuffer[8] == 'M')
							   k = 12;
						   else
							if (readBuffer[8] == 'N')
							   k = 13;
						   else
							if (readBuffer[8] == 'O')
							   k = 14;
						   else
							if (readBuffer[8] == 'P')
							   k = 15;
						   else
							if (readBuffer[8] == 'Q')
							   k = 16;
						   else
							if (readBuffer[8] == 'R')
							   k = 17;
						   else
							if (readBuffer[8] == 'S')
							   k = 18;
						   else
							if (readBuffer[8] == 'T')
							   k = 19;
						   else
							if (readBuffer[8] == 'U')
							   k = 20;
						   else
							if (readBuffer[8] == 'V')
							   k = 21;
						   else
							if (readBuffer[8] == 'W')
							   k = 22;
						   else
							if (readBuffer[8] == 'X')
							   k = 23;
						   else
							if (readBuffer[8] == 'Y')
							   k = 24;
							else
							   k = 25;

                           for (j = 0; j < 26; j++)
                           {
                              if (i == j)
                              {
                                 /*** inform the remote XRF reflector: start ***/
                                 if (a_user_ptr->is_xrf)
                                 {
                                    if (a_user_ptr->rpt_mods[i][k] != readBuffer[8])
                                    {
                                       memcpy(reply_to_xrf, OWNER, 8);
                                       reply_to_xrf[8] = readBuffer[9];
                                       reply_to_xrf[9] = readBuffer[8];
                                       reply_to_xrf[10] = '\0';
                                       for (n = 0; n < 5; n++)
                                          sendto(srv_sock, reply_to_xrf, CALL_SIZE + 3,
                                                 0,(struct sockaddr *)&(a_user_ptr->sin),
                                                 sizeof(struct sockaddr_in)); 
                                    }
                                 }
                                 /*** inform the remote XRF reflector: end ***/

                                 a_user_ptr->rpt_mods[i][k] = readBuffer[8];
                                 time(&a_user_ptr->link_time[i][k]); 
                              }
                              else
                              {
                                 a_user_ptr->rpt_mods[j][k] = '\0';
                                 a_user_ptr->link_time[j][k] = 0;
                              }
                           }
                             
                           /*** 
                           printf("Link established from %s mod %c ---> mod %c\n",
                                   a_user_ptr->call, readBuffer[8], readBuffer[9]);
                           ***/

                           print_links_file();

                           if (!a_user_ptr->is_xrf)
                           {
                              memcpy(readBuffer + 10, "ACK", 4);
                              sendto(srv_sock, readBuffer, CALL_SIZE + 6,
                                     0,(struct sockaddr *)&(a_user_ptr->sin),
                                     sizeof(struct sockaddr_in));
                           }
                        }
                     }
                  }
               }
            }
            else
            {
               allowed_to_connect = true;
               // printf("Possible new connection: %.*s ip=%s\n",  recvlen - 1, readBuffer, an_ip);

               do 
               {
                  /* ONWER can not connect to itself */
                  if (memcmp(a_call,OWNER,CALL_SIZE) == 0)
                  {
                     allowed_to_connect = false;
                     break;
                  }

                  /* reached capacity */ 
                  if (a_user_list.size() + 1 > MAX_USERS)
                  {
                     // printf("Reached MAX_USERS capacity\n");
                     allowed_to_connect = false;
                     break;
                  }
              
                  /* Is this callsign blocked? */ 
                  block_pos = blocks.find(a_call);
                  if (block_pos != blocks.end())
                  {
                     // printf("Callsign is blocked\n");
                     allowed_to_connect = false;
                     break;
                  }

                  /* if it is XRF, do we want to accept it? */
                  if (memcmp(a_call, "XRF", 3) == 0) 
                  {
                     pos = call_ip_map.find(a_call);
                     if (pos == call_ip_map.end())
                     {
                        allowed_to_connect = false;
                        break;
                     }
                  } 
                  
               } while (false);

               if (allowed_to_connect)
               {
                  /* do we have a repeater in the database? */
                  call_valid = regexec(&preg, a_call, 0, NULL, 0);

                  if (recvlen == (CALL_SIZE + 3))
                  {
                     if ((call_valid != REG_NOERROR) && (memcmp(a_call, "XRF", 3) != 0))
                     {
                        // printf("Received link information from %s %s but call is not valid\n", a_call, an_ip);
                        allowed_to_connect = false;
                     }
                     else
                     {
                        /* reflector module */
                        if ((readBuffer[9] != 'A') && (readBuffer[9] != 'B') && (readBuffer[9] != 'C') &&
							(readBuffer[9] != 'D') && (readBuffer[9] != 'E') && (readBuffer[9] != 'F') &&
							(readBuffer[9] != 'G') && (readBuffer[9] != 'H') && (readBuffer[9] != 'I') &&
							(readBuffer[9] != 'J') && (readBuffer[9] != 'K') && (readBuffer[9] != 'L') &&
							(readBuffer[9] != 'M') && (readBuffer[9] != 'N') && (readBuffer[9] != 'O') &&
							(readBuffer[9] != 'P') && (readBuffer[9] != 'Q') && (readBuffer[9] != 'R') &&
							(readBuffer[9] != 'S') && (readBuffer[9] != 'T') && (readBuffer[9] != 'U') &&
							(readBuffer[9] != 'V') && (readBuffer[9] != 'W') && (readBuffer[9] != 'X') &&
                            (readBuffer[9] != 'Y') && (readBuffer[9] != 'Z'))
                           allowed_to_connect = false; 
                        else
                        /* repeater band */
                        if ((readBuffer[8] != 'A') && (readBuffer[8] != 'B') && (readBuffer[8] != 'C') &&
							(readBuffer[8] != 'D') && (readBuffer[8] != 'E') && (readBuffer[8] != 'F') &&
							(readBuffer[8] != 'G') && (readBuffer[8] != 'H') && (readBuffer[8] != 'I') &&
							(readBuffer[8] != 'J') && (readBuffer[8] != 'K') && (readBuffer[8] != 'L') &&
							(readBuffer[8] != 'M') && (readBuffer[8] != 'N') && (readBuffer[8] != 'O') &&
							(readBuffer[8] != 'P') && (readBuffer[8] != 'Q') && (readBuffer[8] != 'R') &&
							(readBuffer[8] != 'S') && (readBuffer[8] != 'T') && (readBuffer[8] != 'U') &&
							(readBuffer[8] != 'V') && (readBuffer[8] != 'W') && (readBuffer[8] != 'X') &&
                            (readBuffer[8] != 'Y') && (readBuffer[8] != 'Z'))
                           allowed_to_connect = false; 
                     }
                  }
                  else
                     allowed_to_connect = false;
               }

               if (allowed_to_connect)
               {
                  /* about to add the new user to the BST/map */
                  a_user_ptr = (struct a_user *)malloc(sizeof(struct a_user));
                  if (a_user_ptr)
                  {
                     /* Initialize the data for the new user */
                     a_user_ptr->countdown = TIMEOUT;
                     memcpy((char *)&(a_user_ptr->sin), (char *)&fromUser, sizeof(struct sockaddr_in));
                     strncpy(a_user_ptr->call, a_call, CALL_SIZE);
                     a_user_ptr->call[CALL_SIZE] = '\0';

                     a_user_ptr->isMute = false;

                     time(&(a_user_ptr->connect_time));

                     if (memcmp(a_user_ptr->call, "XRF", 3) == 0)
                        a_user_ptr->is_xrf = true;
                     else
                        a_user_ptr->is_xrf = false;

                     /* w1bsb: a_user_ptr->sin.sin_port = htons(LISTEN_PORT); */

                     /* Initialize links */
                     for (i = 0; i < 26; i++)
                     {
                        for (k = 0; k < 25; k++)
                        {
                           a_user_ptr->rpt_mods[i][k] = '\0';
                           a_user_ptr->link_time[i][k] = 0;
                        }
                     }

                     a_user_ptr->mod = ' ';

                     if (recvlen == (CALL_SIZE + 3))
                     {
                        /* 
                           A repeater requesting a specific link. 
                           set the link corrrectly.
                        */

                        /* local reflector module */
						if (readBuffer[9] == 'A')
						   i = 0;
						else
						if (readBuffer[9] == 'B')
						   i = 1;
						else
						if (readBuffer[9] == 'C')
						   i = 2;
					   else
						if (readBuffer[9] == 'D')
						   i = 3;
					   else
						if (readBuffer[9] == 'E')
						   i = 4;
					   else
						if (readBuffer[9] == 'F')
						   i = 5;
					   else
						if (readBuffer[9] == 'G')
						   i = 6;
					   else
						if (readBuffer[9] == 'H')
						   i = 7;
					   else
						if (readBuffer[9] == 'I')
						   i = 8;
					   else
						if (readBuffer[9] == 'J')
						   i = 9;
					   else
						if (readBuffer[9] == 'K')
						   i = 10;
					   else
						if (readBuffer[9] == 'L')
						   i = 11;
					   else
						if (readBuffer[9] == 'M')
						   i = 12;
					   else
						if (readBuffer[9] == 'N')
						   i = 13;
					   else
						if (readBuffer[9] == 'O')
						   i = 14;
					   else
						if (readBuffer[9] == 'P')
						   i = 15;
					   else
						if (readBuffer[9] == 'Q')
						   i = 16;
					   else
						if (readBuffer[9] == 'R')
						   i = 17;
					   else
						if (readBuffer[9] == 'S')
						   i = 18;
					   else
						if (readBuffer[9] == 'T')
						   i = 19;
					   else
						if (readBuffer[9] == 'U')
						   i = 20;
					   else
						if (readBuffer[9] == 'V')
						   i = 21;
					   else
						if (readBuffer[9] == 'W')
						   i = 22;
					   else
						if (readBuffer[9] == 'X')
						   i = 23;
					   else
						if (readBuffer[9] == 'Y')
						   i = 24;
						else
						   i = 25;

					   /* remote repeater band */
						if (readBuffer[8] == 'A')
						   k = 0;
						else
						if (readBuffer[8] == 'B')
						   k = 1;
						else
						if (readBuffer[8] == 'C')
						   k = 2;
					   else
						if (readBuffer[8] == 'D')
						   k = 3;
					   else
						if (readBuffer[8] == 'E')
						   k = 4;
					   else
						if (readBuffer[8] == 'F')
						   k = 5;
					   else
						if (readBuffer[8] == 'G')
						   k = 6;
					   else
						if (readBuffer[8] == 'H')
						   k = 7;
					   else
						if (readBuffer[8] == 'I')
						   k = 8;
					   else
						if (readBuffer[8] == 'J')
						   k = 9;
					   else
						if (readBuffer[8] == 'K')
						   k = 10;
					   else
						if (readBuffer[8] == 'L')
						   k = 11;
					   else
						if (readBuffer[8] == 'M')
						   k = 12;
					   else
						if (readBuffer[8] == 'N')
						   k = 13;
					   else
						if (readBuffer[8] == 'O')
						   k = 14;
					   else
						if (readBuffer[8] == 'P')
						   k = 15;
					   else
						if (readBuffer[8] == 'Q')
						   k = 16;
					   else
						if (readBuffer[8] == 'R')
						   k = 17;
					   else
						if (readBuffer[8] == 'S')
						   k = 18;
					   else
						if (readBuffer[8] == 'T')
						   k = 19;
					   else
						if (readBuffer[8] == 'U')
						   k = 20;
					   else
						if (readBuffer[8] == 'V')
						   k = 21;
					   else
						if (readBuffer[8] == 'W')
						   k = 22;
					   else
						if (readBuffer[8] == 'X')
						   k = 23;
					   else
						if (readBuffer[8] == 'Y')
						   k = 24;
						else
						   k = 25;

                        a_user_ptr->rpt_mods[i][k] = readBuffer[8];
                        time(&a_user_ptr->link_time[i][k]);

                     }

                     /* Add new user to the connected list */
                     user_insert_pair = a_user_list.insert(pair<string, struct a_user *>(an_ip, a_user_ptr)); 
                     if (user_insert_pair.second) 
                     {
                        printf("new CALL=%s,REPEATER,ip=%s,users=%ld, link from remote mod %c ---> local mod %c\n",
                                a_call,
                                an_ip,
                                a_user_list.size() + inbound_list.size(),
                                readBuffer[8], readBuffer[9]);

                        print_links_file();

                        if (!a_user_ptr->is_xrf)
                        {
                           memcpy(readBuffer + 10, "ACK", 4);
                           sendto(srv_sock, readBuffer, CALL_SIZE + 6,
                                  0,(struct sockaddr *)&(a_user_ptr->sin),
                                  sizeof(struct sockaddr_in));
                        }
                        else
                        {
                           /*** inform the remote XRF reflector: start ***/
                           memcpy(reply_to_xrf, OWNER, 8);
                           reply_to_xrf[8] = readBuffer[9];
                           reply_to_xrf[9] = readBuffer[8];
                           reply_to_xrf[10] = '\0';
                           for (n = 0; n < 26; n++)
                              sendto(srv_sock, reply_to_xrf, CALL_SIZE + 3,
                                     0,(struct sockaddr *)&(a_user_ptr->sin),
                                     sizeof(struct sockaddr_in));
                           /*** inform the remote XRF reflector: end ***/
                        }
                     }
                     else
                     {
                        printf("Failed to add CALL=%s,ip=%s\n",a_call,an_ip);

                        /* If it was a link request, send back a NAK */
                        memcpy(readBuffer + 10, "NAK", 4);
                        sendto(srv_sock, readBuffer, CALL_SIZE + 6,
                               0,(struct sockaddr *)&(a_user_ptr->sin),
                               sizeof(struct sockaddr_in));

                        free(a_user_ptr);
                        a_user_ptr = NULL;
                     }
                  }
                  else
                  {
                     printf("malloc() failed for call=%s,ip=%s\n",a_call, an_ip);

                     /* If it was a link request, send back a NAK */
                     if (recvlen == (CALL_SIZE + 3))
                     {
                        memcpy(readBuffer + 10, "NAK", 4);

                        /* w1bsb: fromUser.sin_port = htons(LISTEN_PORT); */

                        sendto(srv_sock, readBuffer, CALL_SIZE + 6,
                               0,(struct sockaddr *)&fromUser,
                               sizeof(struct sockaddr_in));
                     }
                  }
               }
               else
               {  
                  /* 
                     If it was a link request, send back a NAK.
                     If it was an unlink request, do nothing 
                  */
                  if (recvlen == (CALL_SIZE + 3))
                  {
                     if ((readBuffer[8] != ' ') && (readBuffer[9] == ' '))
                        ;
                     else
                     {
                        memcpy(readBuffer + 10, "NAK", 4);

                        /* fromUser.sin_port = htons(LISTEN_PORT); */

                        sendto(srv_sock, readBuffer, CALL_SIZE + 6,
                               0,(struct sockaddr *)&fromUser,
                               sizeof(struct sockaddr_in));
                     }
                  }
               }
            }
         }
         else
         if ((recvlen == 56) || 
             (recvlen == 27))
         {
            found = false;

            user_pos = a_user_list.find(an_ip);
            if (user_pos != a_user_list.end())
            {
               a_user_ptr = (struct a_user *)user_pos->second;
               a_user_ptr->countdown = TIMEOUT;

               if (!a_user_ptr->isMute)
                  found = true;

               if (recvlen == 56)
               {
                  memcpy(a_call, readBuffer + 42, CALL_SIZE);
                  a_call[CALL_SIZE] = '\0';

                  call_valid = regexec(&preg, a_call, 0, NULL, 0);
                  if (call_valid != REG_NOERROR) 
                  {
                     printf("User callsign [%s] not valid, audio from [%s] will not be transmitted\n", a_call, an_ip);
                     found = false;
                  }
                  else
                  {
                     a_call[7] = ' ';
                     if (blocks.find(a_call) != blocks.end())
                     {
                        printf("User callsign [%s] blocked, audio from [%s] will not be transmitted\n", a_call, an_ip);
                        found = false;
                     }
                  }

                  /* last check, to make sure audio is from a valid linked source band */
                  if (found)
                  {
                     // source band must be one of: A B C D
                     source_mod = '?'; // assume bad source module

                     if (a_user_ptr->is_xrf)
                     {
                        /* a remote reflector sends its own data */
                        /* remote band of reflector */
						if (readBuffer[25] == 'A')
						   k = 0;
						else
						if (readBuffer[25] == 'B')
						   k = 1;
						else
						if (readBuffer[25] == 'C')
						   k = 2;
					   else
						if (readBuffer[25] == 'D')
						   k = 3;
					   else
						if (readBuffer[25] == 'E')
						   k = 4;
					   else
						if (readBuffer[25] == 'F')
						   k = 5;
					   else
						if (readBuffer[25] == 'G')
						   k = 6;
					   else
						if (readBuffer[25] == 'H')
						   k = 7;
					   else
						if (readBuffer[25] == 'I')
						   k = 8;
					   else
						if (readBuffer[25] == 'J')
						   k = 9;
					   else
						if (readBuffer[25] == 'K')
						   k = 10;
					   else
						if (readBuffer[25] == 'L')
						   k = 11;
					   else
						if (readBuffer[25] == 'M')
						   k = 12;
					   else
						if (readBuffer[25] == 'N')
						   k = 13;
					   else
						if (readBuffer[25] == 'O')
						   k = 14;
					   else
						if (readBuffer[25] == 'P')
						   k = 15;
					   else
						if (readBuffer[25] == 'Q')
						   k = 16;
					   else
						if (readBuffer[25] == 'R')
						   k = 17;
					   else
						if (readBuffer[25] == 'S')
						   k = 18;
					   else
						if (readBuffer[25] == 'T')
						   k = 19;
					   else
						if (readBuffer[25] == 'U')
						   k = 20;
					   else
						if (readBuffer[25] == 'V')
						   k = 21;
					   else
						if (readBuffer[25] == 'W')
						   k = 22;
					   else
						if (readBuffer[25] == 'X')
						   k = 23;
					   else
						if (readBuffer[25] == 'Y')
						   k = 24;
						else
						if (readBuffer[25] == 'Z')
						   k = 25;
                        else
                           k = -1;

                        if (k >= 0)
                        {
                           // find our local module
                           for (i = 0; i < 26; i++)
                           {
                              if (a_user_ptr->rpt_mods[i][k] == readBuffer[25])
                              {
                                 source_mod = readBuffer[25];  // band of remote reflector

                                 /* set rpt2 */
                                 memcpy(readBuffer + 26, OWNER, OWNER_SIZE);
                                 readBuffer[33] = 'G';

                                 /* set rpt1 */
                                 memcpy(readBuffer + 18, OWNER, 7);

                                 // set our local module
                                 if (i == 0)
                                    readBuffer[25] = 'A';
                                 else
                                 if (i == 1)
                                    readBuffer[25] = 'B';
                                 else
                                 if (i == 2)
                                    readBuffer[25] = 'C';
                                 else
                                 if (i == 3)
                                    readBuffer[25] = 'D';
								 else
                                 if (i == 4)
                                    readBuffer[25] = 'E';
								 else
                                 if (i == 5)
                                    readBuffer[25] = 'F';
								 else
                                 if (i == 6)
                                    readBuffer[25] = 'G';
								 else
                                 if (i == 7)
                                    readBuffer[25] = 'H';
								 else
                                 if (i == 8)
                                    readBuffer[25] = 'I';
								 else
                                 if (i == 9)
                                    readBuffer[25] = 'J';
								 else
                                 if (i == 10)
                                    readBuffer[25] = 'K';
								 else
                                 if (i == 11)
                                    readBuffer[25] = 'L';
								 else
                                 if (i == 12)
                                    readBuffer[25] = 'M';
								 else
                                 if (i == 13)
                                    readBuffer[25] = 'N';
								 else
                                 if (i == 14)
                                    readBuffer[25] = 'O';
								 else
                                 if (i == 15)
                                    readBuffer[25] = 'P';
								 else
                                 if (i == 16)
                                    readBuffer[25] = 'Q';
								 else
                                 if (i == 17)
                                    readBuffer[25] = 'R';
								 else
                                 if (i == 18)
                                    readBuffer[25] = 'S';
								 else
                                 if (i == 19)
                                    readBuffer[25] = 'T';
								 else
                                 if (i == 20)
                                    readBuffer[25] = 'U';
								 else
                                 if (i == 21)
                                    readBuffer[25] = 'V';
								 else
                                 if (i == 22)
                                    readBuffer[25] = 'W';
								 else
                                 if (i == 23)
                                    readBuffer[25] = 'X';
								 else
                                 if (i == 24)
                                    readBuffer[25] = 'Y';
                                 else
                                    readBuffer[25] = 'Z';
                     
                                 break;
                     
                              }
                           } 
                        }
                     }
                     else
                     {
                        /*
                        A remote repeater sends our data.
                        rpt1 contains our reflector module, we can NOT use that,
                        we have to find the band of the repeater.
                        Since a repeater may have more than one band linked to our reflector module,
                        we will use byte 11  as incoming source band.
                        If the repeater did not use g2_link to link to our reflector,
                          then that repeater must be modified to support the new XRF features of multilinking,
                        */
                        if ((readBuffer[11] >= 'A') && (readBuffer[11] <= 'D'))
                           source_mod = readBuffer[11]; // band of remote repeater
                        else
                        {
                           /* repeater is not using g2_link, try this */

                           /* byte 25 is our reflector mod */
							if (readBuffer[9] == 'A')
							   i = 0;
							else
							if (readBuffer[9] == 'B')
							   i = 1;
							else
							if (readBuffer[9] == 'C')
							   i = 2;
						   else
							if (readBuffer[9] == 'D')
							   i = 3;
						   else
							if (readBuffer[9] == 'E')
							   i = 4;
						   else
							if (readBuffer[9] == 'F')
							   i = 5;
						   else
							if (readBuffer[9] == 'G')
							   i = 6;
						   else
							if (readBuffer[9] == 'H')
							   i = 7;
						   else
							if (readBuffer[9] == 'I')
							   i = 8;
						   else
							if (readBuffer[9] == 'J')
							   i = 9;
						   else
							if (readBuffer[9] == 'K')
							   i = 10;
						   else
							if (readBuffer[9] == 'L')
							   i = 11;
						   else
							if (readBuffer[9] == 'M')
							   i = 12;
						   else
							if (readBuffer[9] == 'N')
							   i = 13;
						   else
							if (readBuffer[9] == 'O')
							   i = 14;
						   else
							if (readBuffer[9] == 'P')
							   i = 15;
						   else
							if (readBuffer[9] == 'Q')
							   i = 16;
						   else
							if (readBuffer[9] == 'R')
							   i = 17;
						   else
							if (readBuffer[9] == 'S')
							   i = 18;
						   else
							if (readBuffer[9] == 'T')
							   i = 19;
						   else
							if (readBuffer[9] == 'U')
							   i = 20;
						   else
							if (readBuffer[9] == 'V')
							   i = 21;
						   else
							if (readBuffer[9] == 'W')
							   i = 22;
						   else
							if (readBuffer[9] == 'X')
							   i = 23;
						   else
							if (readBuffer[9] == 'Y')
							   i = 24;
							else
							if (readBuffer[9] == 'Z')
							   i = 25;
                           else
                              i = -1;

                           /* now find the first band of the remote repeater linked to our reflector mod */
                           if (i >= 0)
                           {
                              for (k = 0; k < 25; k++)
                              {
                                 if (a_user_ptr->rpt_mods[i][k] != '\0')
                                 {
                                    source_mod = a_user_ptr->rpt_mods[i][k];  /* we found it */

                                    if (readBuffer[33] == 'G')
                                    {
                                       memcpy(readBuffer + 18, OWNER, 7);
                                       memcpy(readBuffer + 26, OWNER, 7);
                                    }
                                    else
                                    if (readBuffer[25] == 'G')
                                    {
                                       readBuffer[25] = readBuffer[33];
                                       readBuffer[33] = 'G';
                                       memcpy(readBuffer + 18, OWNER, 7);
                                       memcpy(readBuffer + 26, OWNER, 7);
                                    }
                                    else
                                    {
                                       memcpy(readBuffer + 18, OWNER, 7);
                                       memcpy(readBuffer + 26, OWNER, 7);
                                       readBuffer[33] = 'G';
                                    }
                                    break;
                                 }
                              }
                           }
                        }
                     }
 
                     if ((source_mod >= 'A') && (source_mod <= 'D'))
                        ;
                     else
                     {
                        if (QSO_DETAILS)
                           printf("Invalid streamID: %d,%d, my=%.8s, rpt1=%.8s, rpt2=%.8s,  %d bytes fromIP=%s\n",
                                    readBuffer[12],readBuffer[13],
                                    &readBuffer[42], &readBuffer[18], &readBuffer[26], recvlen, an_ip);
                        found = false;
                     }
                  }
               }
            }

            if (found)
            {
               sprintf(an_rcd_streamid,"%s-%d,%d", an_ip, readBuffer[12], readBuffer[13]);
               if (recvlen == 56)
               {
                  if (QSO_DETAILS)
                     printf("START user: streamID=%d,%d, my=%.8s, sfx=%.4s, ur=%.8s, rpt1=%.8s, rpt2=%.8s, %d bytes fromIP=%s, src=%.8s %c\n",
                             readBuffer[12],readBuffer[13],&readBuffer[42], 
                             &readBuffer[50], &readBuffer[34],
                             &readBuffer[18], &readBuffer[26],
                             recvlen, an_ip,
                             a_user_ptr->call, source_mod);

                  memcpy(tmp1, readBuffer + 42, 8); tmp1[8] = '\0';

                  // delete the user if exists
                  for (dt_lh_pos = dt_lh_list.begin(); dt_lh_pos != dt_lh_list.end();  dt_lh_pos++)
                  {
                     if (strcmp((char *)dt_lh_pos->second.c_str(), tmp1) == 0)
                     {
                        dt_lh_list.erase(dt_lh_pos);
                        break;
                     }
                  }
                  /* Limit?, delete oldest user */
                  if (dt_lh_list.size() == LH_MAX_SIZE)
                  {
                     dt_lh_pos = dt_lh_list.begin();
                     dt_lh_list.erase(dt_lh_pos);
                  }
                  time(&tNow);
                  sprintf(tmp2, "%ld=%.6s%c%c", tNow,  a_user_ptr->call, source_mod, readBuffer[25]);
                  dt_lh_list[tmp2] = tmp1;

                  /* This user is talking on this module */
                  a_user_ptr->mod = readBuffer[25];

                  if (readBuffer[25] == 'E')
                  {
                     rcd_pos = rcd_list.find(an_rcd_streamid);
                     if (rcd_pos == rcd_list.end())
                     {
                        if (rcd_list.size() < MAX_RCD_USERS)
                        {
                           an_rcd = (struct rcd *)malloc(sizeof(struct rcd));
                           if (an_rcd)
                           {
                              an_rcd->locked = false;
                              time(&(an_rcd->ts)); /* stamp it */
                              memcpy((char *)&(an_rcd->sin), (char *)&(a_user_ptr->sin), sizeof(struct sockaddr_in));
                              memcpy(an_rcd->data[0], readBuffer, recvlen);
                              an_rcd->recvlen = recvlen;
                              an_rcd->idx = 1;
                              rcd_insert_pair = rcd_list.insert(pair<string, struct rcd *>(an_rcd_streamid, an_rcd));
                              if (rcd_insert_pair.second)
                                 ; // printf("Started recording for user %.8s, streamid=%d,%d\n",
                                   //         readBuffer + 42, readBuffer[12], readBuffer[13]);
                              else
                              {
                                 printf("failed to add user %.8s, streamid=%d,%d, for recording...\n",
                                         readBuffer + 42, readBuffer[12], readBuffer[13]);
                                 free(an_rcd);
                                 an_rcd = NULL;
                              }
                           }
                           else
                              printf("Failed to allocate for recording on user %.8s, streamid=%d,%d\n",
                                      readBuffer + 42, readBuffer[12], readBuffer[13]);
                        }
                        else
                           printf("Reached maximum concurrent users for recording\n");
                     }
                  }
               }
               else
               {
                  if ((readBuffer[14] & 0x40) != 0)
                  {
                     if (QSO_DETAILS)
                        printf("END user: streamID=%d,%d, %d bytes from IP=%s\n",
                                 readBuffer[12],readBuffer[13],recvlen, an_ip);
                  }

                  rcd_pos = rcd_list.find(an_rcd_streamid);
                  if (rcd_pos != rcd_list.end())
                  {
                     an_rcd = (struct rcd *)rcd_pos->second;
                     time(&(an_rcd->ts)); /* stamp it */
                     if ((an_rcd->idx < MAX_RCD_DATA) && !(an_rcd->locked))
                     {
                        memcpy(an_rcd->data[an_rcd->idx], readBuffer, recvlen);
                        an_rcd->idx ++;
                     }
                     if (((readBuffer[14] & 0x40) != 0) && !(an_rcd->locked))
                     {
                        /***
                        printf("Finished recording for user %.8s, streamid=%d,%d\n",
                                 &(an_rcd->data[0][42]), readBuffer[12], readBuffer[13]);
                        ***/

                        /*** start playback */
                        an_rcd->locked = true;
                        pthread_attr_init(&attr);
                        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
                        rc = pthread_create(&playback_thread,&attr,playback,(void *)rcd_pos->second);
                        if (rc != 0)
                        {
                           printf("Failed to start the playback thread for streamid=%d,%d\n",
                                    readBuffer[12], readBuffer[13]);
                           free(rcd_pos->second);
                           rcd_pos->second = NULL;
                           rcd_list.erase(rcd_pos);
                        }
                        pthread_attr_destroy(&attr);
                     }
                  }
               }

               /* prepare to send the data to dvap, dvtool stations */
               refbuf[0] = (unsigned char)((recvlen + 2) & 0xFF);
               refbuf[1] = (unsigned char)((recvlen + 2) >> 8 & 0x1F);
               refbuf[1] = (unsigned char)(refbuf[1] | 0xFFFFFF80);
               memcpy(refbuf + 2, readBuffer, recvlen);

               /* At this point, if this is a header, it contains our reflector info */
               /* save the header for re-transmit */
               if (recvlen == 56)
               {
                  /* save it */
                  source_mod = refbuf[27];

                  i = -1;
				  if (refbuf[27] == 'A')
					 i = 0;
				  else
				  if (refbuf[27] == 'B')
					 i = 1;
				  else
				  if (refbuf[27] == 'C')
					 i = 2;
				  else
				  if (refbuf[27] == 'D')
					 i = 3;
				 else
				  if (refbuf[27] == 'E)
					 i = 4;
				 else
				  if (refbuf[27] == 'F')
					 i = 5;
				 else
				  if (refbuf[27] == 'G')
					 i = 6;
				 else
				  if (refbuf[27] == 'H')
					 i = 7;
				 else
				  if (refbuf[27] == 'I')
					 i = 8;
				 else
				  if (refbuf[27] == 'J')
					 i = 9;
				 else
				  if (refbuf[27] == 'K')
					 i = 10;
				 else
				  if (refbuf[27] == 'L')
					 i = 11;
				 else
				  if (refbuf[27] == 'M')
					 i = 12;
				 else
				  if (refbuf[27] == 'N')
					 i = 13;
				 else
				  if (refbuf[27] == 'O')
					 i = 14;
				 else
				  if (refbuf[27] == 'P')
					 i = 15;
				 else
				  if (refbuf[27] == 'Q')
					 i = 16;
				 else
				  if (refbuf[27] == 'R')
					 i = 17;
				 else
				  if (refbuf[27] == 'S')
					 i = 18;
				 else
				  if (refbuf[27] == 'T')
					 i = 19;
				 else
				  if (refbuf[27] == 'U')
					 i = 20;
				 else
				  if (refbuf[27] == 'V')
					 i = 21;
				 else
				  if (refbuf[27] == 'W')
					 i = 22;
				 else
				  if (refbuf[27] == 'X')
					 i = 23;
				 else
				  if (refbuf[27] == 'Y')
					 i = 24;
				 else
				  if (refbuf[27] == 'Z')
					 i = 25;

                  if (i >= 0)
                  {
                     temp_r[i].s_addr = fromUser.sin_addr.s_addr;
                     memcpy(temp_r[i].hdr, refbuf, 58);
                  }
               }

               /* send the data to dvap, dvtool stations */
               if ((inbound_list.size() > 0) && (rcd_list.find(an_rcd_streamid) == rcd_list.end()))
               {
                  for (inbound_pos2 = inbound_list.begin(); inbound_pos2 != inbound_list.end(); inbound_pos2++)
                  {
                     inbound_ptr2 = (inbound *)inbound_pos2->second;
                     if (recvlen == 56)
                     {
                        if (inbound_ptr2->is_ref)
                        {
                           if (i >= 0)
                           {
                              if (inbound_ptr2->links[i] != ' ')
                              {
                                 memcpy(refbuf + 20, inbound_ptr2->call, 7);
                                 refbuf[27] = inbound_ptr2->links[i];
                                 memcpy(refbuf + 28, inbound_ptr2->call, 7);

                                 sendto(ref_sock, (char *)refbuf, recvlen + 2, 0,
                                        (struct sockaddr *)&(inbound_ptr2->sin),
                                        sizeof(struct sockaddr_in));
                              }
                           }
                        }
                        else
                        {
                           /* reset it */
                           memcpy(refbuf + 20, OWNER, 7);
                           refbuf[27] = source_mod;
                           memcpy(refbuf + 28, OWNER, 7);

                           if (i >= 0)
                              sendto(ref_sock, (char *)refbuf, recvlen + 2, 0,
                                     (struct sockaddr *)&(inbound_ptr2->sin),
                                     sizeof(struct sockaddr_in));
                        }
                     }
                     else
                     {
                        if ((refbuf[26] == 0x55) && (refbuf[27] == 0x2d) && (refbuf[28] == 0x16))
                        {
                           if (!inbound_ptr2->is_ref)
                           {
                              for (i = 0; i < 25; i++)
                              {
                                 if ((refbuf[14] == temp_r[i].hdr[14]) &&
                                     (refbuf[15] == temp_r[i].hdr[15]) &&
                                     (fromUser.sin_addr.s_addr == temp_r[i].s_addr))
                                 {
                                    sendto(ref_sock, (char *)temp_r[i].hdr, 58, 0,
                                           (struct sockaddr *)&(inbound_ptr2->sin), sizeof(struct sockaddr_in));
                                    break;
                                 }
                              }
                           }
                        }

                        /* if it is a regular dongle, send it */
                        if (!inbound_ptr2->is_ref)
                           sendto(ref_sock, (char *)refbuf, recvlen + 2, 0,
                                  (struct sockaddr *)&(inbound_ptr2->sin),
                                  sizeof(struct sockaddr_in));
                        else
                        {
                           /* treat the remote reflector with resepct */
                           /* send it only if the module matches */
                           for (i = 0; i < 25; i++)
                           {
                              if ((refbuf[14] == temp_r[i].hdr[14]) &&
                                  (refbuf[15] == temp_r[i].hdr[15]) &&
                                  (fromUser.sin_addr.s_addr == temp_r[i].s_addr) &&
                                  (inbound_ptr2->links[i] != ' '))
                              {
                                 sendto(ref_sock, (char *)refbuf, recvlen + 2, 0,
                                        (struct sockaddr *)&(inbound_ptr2->sin),
                                        sizeof(struct sockaddr_in));

                                 break;
                              }
                           }
                        }
                     }
                  }
               }

               if (rcd_list.find(an_rcd_streamid) == rcd_list.end())
               {
/****************************** REPLACEMENT to send_a_pkt_to_all ****************************************************/
                  if (recvlen == 56)
                  {
					  if (readBuffer[25] == 'A')
						 k = 0;
					  else
					  if (readBuffer[25] == 'B')
						 k = 1;
					  else
					  if (readBuffer[25] == 'C')
						 k = 2;
					  else
					  if (readBuffer[25] == 'D')
						 k = 3;
					 else
					  if (readBuffer[25] == 'E)
						 k = 4;
					 else
					  if (readBuffer[25] == 'F')
						 k = 5;
					 else
					  if (readBuffer[25] == 'G')
						 k = 6;
					 else
					  if (readBuffer[25] == 'H')
						 k = 7;
					 else
					  if (readBuffer[25] == 'I')
						 k = 8;
					 else
					  if (readBuffer[25] == 'J')
						 k = 9;
					 else
					  if (readBuffer[25] == 'K')
						 k = 10;
					 else
					  if (readBuffer[25] == 'L')
						 k = 11;
					 else
					  if (readBuffer[25] == 'M')
						 k = 12;
					 else
					  if (readBuffer[25] == 'N')
						 k = 13;
					 else
					  if (readBuffer[25] == 'O')
						 k = 14;
					 else
					  if (readBuffer[25] == 'P')
						 k = 15;
					 else
					  if (readBuffer[25] == 'Q')
						 k = 16;
					 else
					  if (readBuffer[25] == 'R')
						 k = 17;
					 else
					  if (readBuffer[25] == 'S')
						 k = 18;
					 else
					  if (readBuffer[25] == 'T')
						 k = 19;
					 else
					  if (readBuffer[25] == 'U')
						 k = 20;
					 else
					  if (readBuffer[25] == 'V')
						 k = 21;
					 else
					  if (readBuffer[25] == 'W')
						 k = 22;
					 else
					  if (readBuffer[25] == 'X')
						 k = 23;
					 else
					  if (readBuffer[25] == 'Y')
						 k = 24;
					 else
					  if (readBuffer[25] == 'Z')
						 k = 25;
                     else
                        k = -1;

                     if (k >= 0)
                     {
                        temp_x[k].old_sid[0] = readBuffer[12];   temp_x[k].old_sid[1] = readBuffer[13];

                        streamid_raw = (readBuffer[12] * 256U) + readBuffer[13];
                        streamid_raw ++;
                        if (streamid_raw == 0)
                           streamid_raw ++;

                        memcpy(temp_x[k].hdr, readBuffer, 56);
                        temp_x[k].hdr[12] = streamid_raw / 256U;
                        temp_x[k].hdr[13] = streamid_raw % 256U;

                        temp_x[k].s_addr = fromUser.sin_addr.s_addr;

                        for (user_pos2 = a_user_list.begin(); user_pos2 != a_user_list.end(); user_pos2++)
                        {
                           a_user_ptr2 = (struct a_user *)user_pos2->second;
                           if (fromUser.sin_addr.s_addr != a_user_ptr2->sin.sin_addr.s_addr)
                           {
                              if ((a_user_ptr2->rpt_mods[k][0] != '\0') ||
                                  (a_user_ptr2->rpt_mods[k][1] != '\0') ||
                                  (a_user_ptr2->rpt_mods[k][2] != '\0') ||
                                  (a_user_ptr2->rpt_mods[k][3] != '\0'))
                                 sendto(srv_sock, (char *)temp_x[k].hdr, recvlen,
                                        0,(struct sockaddr *)&(a_user_ptr2->sin),
                                        sizeof(struct sockaddr_in));
                           }
                        }
                     }
                  }
                  else
                  {
                     for (user_pos2 = a_user_list.begin(); user_pos2 != a_user_list.end(); user_pos2++)
                     {
                        a_user_ptr2 = (struct a_user *)user_pos2->second;
                        if (fromUser.sin_addr.s_addr != a_user_ptr2->sin.sin_addr.s_addr)
                        {
                           /* source must NOT be a reflector */
                           if (!(a_user_ptr->is_xrf))
                           {
                              if ((readBuffer[24] == 0x55) && (readBuffer[25] == 0x2d) && (readBuffer[26] == 0x16))
                              {
                                 for (i = 0; i < 4; i++)
                                 {
                                    if ((readBuffer[12] == temp_x[i].old_sid[0]) &&
                                        (readBuffer[13] == temp_x[i].old_sid[1]) &&
                                        (fromUser.sin_addr.s_addr == temp_x[i].s_addr) &&
                                        ((a_user_ptr2->rpt_mods[i][0] != '\0') || /* module is linked */
                                         (a_user_ptr2->rpt_mods[i][1] != '\0') ||
                                         (a_user_ptr2->rpt_mods[i][2] != '\0') ||
                                         (a_user_ptr2->rpt_mods[i][3] != '\0')))
                                    {
                                       sendto(srv_sock, (char *)temp_x[i].hdr, 56, 0,
                                              (struct sockaddr *)&(a_user_ptr2->sin), sizeof(struct sockaddr_in));
                                       break;
                                    }
                                 }
                              }
                           }

                           for (i = 0; i < 25; i++)
                           {
                              if ((readBuffer[12] == temp_x[i].old_sid[0]) &&
                                  (readBuffer[13] == temp_x[i].old_sid[1]) &&
                                  (fromUser.sin_addr.s_addr == temp_x[i].s_addr) &&
                                  ((a_user_ptr2->rpt_mods[i][0] != '\0') || /* module is linked */
                                   (a_user_ptr2->rpt_mods[i][1] != '\0') ||
                                   (a_user_ptr2->rpt_mods[i][2] != '\0') ||
                                   (a_user_ptr2->rpt_mods[i][3] != '\0')))
                              {
                                 readBuffer[12] = temp_x[i].hdr[12];  readBuffer[13] = temp_x[i].hdr[13];
                                 sendto(srv_sock, (char *)readBuffer, recvlen,
                                        0,(struct sockaddr *)&(a_user_ptr2->sin),
                                        sizeof(struct sockaddr_in));
                                 readBuffer[12] = temp_x[i].old_sid[0]; readBuffer[13] = temp_x[i].old_sid[1];
                                 break;
                              }
                           }
                        } 
                     }
                  }
/********************************************************************************************************************/
               }
            }
         }
         FD_CLR (srv_sock,&fdset);
      }
   }
}

static void *playback(void *arg)
{
   struct rcd *temp_rcd = (struct rcd *)arg;
   struct sigaction act;
   unsigned short int j;
   unsigned short int i;
   struct timespec nanos;

   act.sa_handler = sigCatch;
   sigemptyset(&act.sa_mask);
   act.sa_flags = SA_RESTART;
   if (sigaction(SIGTERM, &act, 0) != 0)
   {
      printf("sigaction-TERM failed, error=%d\n", errno);
      keep_running = false;
      pthread_exit(NULL);
   }
   if (sigaction(SIGINT, &act, 0) != 0)
   {
      printf("sigaction-INT failed, error=%d\n", errno);
      keep_running = false;
      pthread_exit(NULL);
   }

   /* delay 5 seconds here */
   sleep(5);

   /***
   if (temp_rcd->recvlen == 56)
      printf("playback started for user=%.8s, streamid %d,%d\n", 
            &(temp_rcd->data[0][42]), temp_rcd->data[0][12], temp_rcd->data[0][13]);
   else
      printf("playback started for user=%.8s, streamid %d,%d\n",
            &(temp_rcd->data[0][44]), temp_rcd->data[0][14], temp_rcd->data[0][15]);
   ***/

   /* first packet is header */

   /*** MUST CHANGE rpt1 and rpt2 if temp_rcd->recvlen is 58 ***/
   for (j = 0; j < 26; j++)
      sendto(ref_sock, (char *)temp_rcd->data[0], temp_rcd->recvlen, 0,
             (struct sockaddr *)&(temp_rcd->sin),
             sizeof(struct sockaddr_in));

   /*** header sent ***/
   i = 1;
   
   while ((keep_running) && (i < temp_rcd->idx))
   {
      time(&(temp_rcd->ts));
      sendto(ref_sock, (char *)temp_rcd->data[i], 
             (temp_rcd->recvlen == 56)?27:29,   /*** ? 32 ***/
             0,(struct sockaddr *)&(temp_rcd->sin),
             sizeof(struct sockaddr_in));
      i ++;
      nanos.tv_sec = 0;
      nanos.tv_nsec = 17000000;
      nanosleep(&nanos,0);
   }

   /***
   if (temp_rcd->recvlen == 56)
      printf("Finished playback for user=%.8s, streamid %d,%d\n",
            &(temp_rcd->data[0][42]), temp_rcd->data[0][12], temp_rcd->data[0][13]);
   else
      printf("Finished playback for user=%.8s, streamid %d,%d\n",
            &(temp_rcd->data[0][44]), temp_rcd->data[0][14], temp_rcd->data[0][15]);
   ***/

   temp_rcd->locked = false;

   pthread_exit(NULL);
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   int rc = 0;
   struct sigaction act;
   inbound_type::iterator inbound_pos; 
   inbound *inbound_ptr;
   short int i;

   if (argc != 2)
   {
      printf("Usage:  dxrfd <configFile>\n");
      printf("Example: dxrfd dxrfd.cfg\n");
      return 1;
   }

   rc = regcomp(&preg,
        "^(([1-9][A-Z])|([A-Z][0-9])|([A-Z][A-Z][0-9]))[0-9A-Z]*[A-Z][ ]*[ A-RT-Z]$",
        REG_EXTENDED | REG_NOSUB);
   if (rc != REG_NOERROR)
   {
      printf("The regular expression is NOT valid\n");
      return 1;
   }

   tzset();

   act.sa_handler = sigCatch;
   sigemptyset(&act.sa_mask);
   act.sa_flags = SA_RESTART;
   if (sigaction(SIGTERM, &act, 0) != 0)
   {
      printf("sigaction-TERM failed, error=%d\n", errno);
      return 1;
   }
   if (sigaction(SIGINT, &act, 0) != 0)
   {
      printf("sigaction-INT failed, error=%d\n", errno);
      return 1;
   }

   do
   {
      /* process configuration file */
      rc = read_config(argv[1]);
      if (rc != 0)
      {
         printf("Failed to process config file %s\n", argv[1]);
         break;
      }

    /* Open DB */
      rc = open_users(USERS);
      if (rc != 0)
      {
         printf("Failed to open %s\n", USERS);
         break;
      }

      rc = open_blocks(BLOCKS);
      if (rc != 0)
      {
         printf("Failed to open %s\n", BLOCKS);
         break;
      }

      /* get ready to accept incoming connections */
      rc = srv_open();
      if (rc != 0)
      {
         printf("srv_open() failed\n");
         break;
      }

      /* get ready to accept shell commands from netcat */
      rc = cmd_open();
      if (rc != 0)
      {
         printf("cmd_open() failed\n");
         break;
      }

      rc = ref_open();
      if (rc != 0)
      {
         printf("ref_open() failed\n");
         break;
      }
  
      for (i = 0; i < 26; i++)
      {
         temp_x[i].s_addr = 0;
         memset(temp_x[i].hdr, 0, 56);

         temp_r[i].s_addr = 0;
         memset(temp_r[i].hdr, 0, 58);
      }

      printf("dxrfd %s initialized...entering processing loop\n", VERSION);

      print_links_file();
      runit();
      printf("Leaving processing loop...\n");

   } while (false);

   /* Close all handles */
   if (srv_sock != -1)
      close(srv_sock);

   if (cmd_sock != -1)
      close(cmd_sock);

   /* notify dvap, dvtool ... */
   if (ref_sock != -1)
   {
      refbuf[0] = 5;
      refbuf[1] = 0;
      refbuf[2] = 24;
      refbuf[3] = 0;
      refbuf[4] = 0;
      for (inbound_pos = inbound_list.begin(); inbound_pos != inbound_list.end(); inbound_pos++)
      {
         inbound_ptr = (inbound *)inbound_pos->second;
         sendto(ref_sock,(char *)refbuf,5,0,
                (struct sockaddr *)&(inbound_ptr->sin),
                sizeof(struct sockaddr_in));
      }
      inbound_list.clear();
      close(ref_sock);
   }

   /* Empty out the status file */
   statusfp = fopen(STATUS_FILE, "w");
   if (statusfp)
      fclose(statusfp);
   else
      printf("Failed to empty out status file %s\n", STATUS_FILE);

   printf("dxrfd exiting\n");
 
   return rc;
}

