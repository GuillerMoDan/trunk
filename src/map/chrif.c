// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/malloc.h"
#include "../common/socket.h"
#include "../common/timer.h"
#include "../common/nullpo.h"
#include "../common/showmsg.h"
#include "../common/strlib.h"
#include "../common/ers.h"

#include "map.h"
#include "battle.h"
#include "clan.h"
#include "clif.h"
#include "intif.h"
#include "npc.h"
#include "pc.h"
#include "pet.h"
#include "skill.h"
#include "status.h"
#include "homunculus.h"
#include "instance.h"
#include "mercenary.h"
#include "elemental.h"
#include "chrif.h"
#include "quest.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

static int check_connect_char_server(int tid, unsigned int tick, int id, intptr_t data);

static struct eri *auth_db_ers; //For reutilizing player login structures.
static DBMap *auth_db; //int id -> struct auth_node*
static bool char_init_done = false; //Server already initialized? Used for InterInitOnce and vending loadings

static const int packet_len_table[0x3d] = { // U - used, F - free
	60, 3,-1,-1,10,-1, 6,-1,	// 2af8-2aff: U->2af8, U->2af9, U->2afa, U->2afb, U->2afc, U->2afd, U->2afe, U->2aff
	 6,-1,19, 7,-1,39,30, 10,	// 2b00-2b07: U->2b00, U->2b01, U->2b02, U->2b03, U->2b04, U->2b05, U->2b06, U->2b07
	 6,30,10,-1,86, 7,40,34,	// 2b08-2b0f: U->2b08, U->2b09, U->2b0a, U->2b0b, U->2b0c, U->2b0d, U->2b0e, U->2b0f
	11,10,10,-1,11,-1,266,10,	// 2b10-2b17: U->2b10, U->2b11, U->2b12, U->2b13, U->2b14, U->2b15, U->2b16, U->2b17
	 2,10, 2,-1,-1,-1, 2, 7,	// 2b18-2b1f: U->2b18, U->2b19, U->2b1a, U->2b1b, U->2b1c, U->2b1d, U->2b1e, U->2b1f
	-1,10, 8, 2, 2,14,19,19,	// 2b20-2b27: U->2b20, U->2b21, U->2b22, U->2b23, U->2b24, U->2b25, U->2b26, U->2b27
	-1, 0, 6,15, 0, 6,-1,-1,	// 2b28-2b2f: U->2b28, F->2b29, U->2b2a, U->2b2b, F->2b2c, U->2b2d, U->2b2e, U->2b2f
};

//Used Packets:
//2af8: Outgoing, chrif_connect -> 'connect to charserver / auth @ charserver'
//2af9: Incoming, chrif_connectack -> 'answer of the 2af8 login(ok / fail)'
//2afa: Outgoing, chrif_sendmap -> 'sending our maps'
//2afb: Incoming, chrif_sendmapack -> 'Maps received successfully / or not... also received server name & default map'
//2afc: Outgoing, chrif_scdata_request -> request sc_data for pc_authok'ed char. <- new command reuses previous one.
//2afd: Incoming, chrif_authok -> 'client authentication ok'
//2afe: Outgoing, send_usercount_tochar -> 'sends player count of this map server to charserver'
//2aff: Outgoing, send_users_tochar -> 'sends all actual connected character ids to charserver'
//2b00: Incoming, map_setusers -> 'set the actual usercount? PACKET.2B COUNT.L.. ?' (not sure)
//2b01: Outgoing, chrif_save -> 'charsave of char XY account XY (complete struct)'
//2b02: Outgoing, chrif_charselectreq -> 'player returns from ingame to charserver to select another char.., this packets includes sessid etc' ? (not 100% sure)
//2b03: Incoming, clif_charselectok -> '' (i think its the packet after enterworld?) (not sure)
//2b04: Incoming, chrif_recvmap -> 'getting maps from charserver of other mapserver's'
//2b05: Outgoing, chrif_changemapserver -> 'Tell the charserver the mapchange / quest for ok...'
//2b06: Incoming, chrif_changemapserverack -> 'awnser of 2b05, ok/fail, data: dunno^^'
//2b07: Outgoing, chrif_removefriend -> 'Tell charserver to remove friend_id from char_id friend list'
//2b08: Outgoing, chrif_searchcharid -> '...'
//2b09: Incoming, map_addchariddb -> 'Adds a name to the nick db'
//2b0a: Outgoing, chrif_skillcooldown_request -> requesting the list of skillcooldown for char
//2b0b: Incoming, chrif_skillcooldown_load -> received the list of cooldown for char
//2b0c: Outgoing, chrif_changeemail -> 'change mail address ...'
//2b0d: Incoming, chrif_changedsex -> 'Change sex of acc XY' (or char)
//2b0e: Outgoing, chrif_req_login_operation -> 'Do some operations (change sex, ban / unban etc)'
//2b0f: Incoming, chrif_ack_login_req -> 'answer of the 2b0e'
//2b10: Outgoing, chrif_updatefamelist -> 'Update the fame ranking lists and send them'
//2b11: Outgoing, chrif_divorce -> 'tell the charserver to do divorce'
//2b12: Incoming, chrif_divorceack -> 'divorce chars
//2b13: Incoming/Outgoing, socket_datasync()
//2b14: Incoming, chrif_accountban -> 'not sure: kick the player with message XY'
//2b15: Outgoing, chrif_skillcooldown_save -> request to save skillcooldown
//2b16: Outgoing, chrif_ragsrvinfo -> 'sends base / job / drop rates ....'
//2b17: Outgoing, chrif_char_offline -> 'tell the charserver that the char is now offline'
//2b18: Outgoing, chrif_char_reset_offline -> 'set all players OFF!'
//2b19: Outgoing, chrif_char_online -> 'tell the charserver that the char .. is online'
//2b1a: Outgoing, chrif_buildfamelist -> 'Build the fame ranking lists and send them'
//2b1b: Incoming, chrif_recvfamelist -> 'Receive fame ranking lists'
//2b1c: Outgoing, chrif_save_scdata -> 'Send sc_data of player for saving.'
//2b1d: Incoming, chrif_load_scdata -> 'received sc_data of player for loading.'
//2b1e: Incoming, chrif_update_ip -> 'Reqest forwarded from char-server for interserver IP sync.' [Lance]
//2b1f: Incoming, chrif_disconnectplayer -> 'disconnects a player (aid X) with the message XY ... 0x81 ..' [Sirius]
//2b20: Incoming, chrif_removemap -> 'remove maps of a server (sample: its going offline)' [Sirius]
//2b21: Incoming, chrif_save_ack. Returned after a character has been "final saved" on the char-server. [Skotlex]
//2b22: Incoming, chrif_updatefamelist_ack. Updated one position in the fame list.
//2b23: Outgoing, chrif_keepalive. charserver ping.
//2b24: Incoming, chrif_keepalive_ack. charserver ping reply.
//2b25: Incoming, chrif_deadopt -> 'Removes baby from Father ID and Mother ID'
//2b26: Outgoing, chrif_authreq -> 'client authentication request'
//2b27: Incoming, chrif_authfail -> 'client authentication failed'
//2b28: Outgoing, chrif_req_charban -> 'ban a specific char'
//2b29: FREE
//2b2a: Outgoing, chrif_req_charunban -> 'unban a specific char'
//2b2b: Incoming, chrif_parse_ack_vipActive -> vip info result
//2b2c: FREE
//2b2d: Outgoing, chrif_bsdata_request -> request bonus_script for pc_authok'ed char.
//2b2e: Outgoing, chrif_bsdata_save -> Send bonus_script of player for saving.
//2b2f: Incoming, chrif_bsdata_received -> received bonus_script of player for loading.

int chrif_connected = 0;
int char_fd = -1;
int srvinfo;
static char char_ip_str[128];
static uint32 char_ip = 0;
static uint16 char_port = 6121;
static char userid[NAME_LENGTH], passwd[NAME_LENGTH];
static int chrif_state = 0;
int other_mapserver_count = 0; //Holds count of how many other map servers are online (apart of this instance) [Skotlex]

//Interval at which map server updates online listing. [Valaris]
#define CHECK_INTERVAL 3600000
//Interval at which map server sends number of connected users. [Skotlex]
#define UPDATE_INTERVAL 10000
//This define should spare writing the check in every function. [Skotlex]
#define chrif_check(a) { if(!chrif_isconnected()) return a; }


/// Resets all the data.
void chrif_reset(void) {
	//@TODO: Kick everyone out and reset everything [FlavioJS]
	exit(EXIT_FAILURE);
}


/// Checks the conditions for the server to stop.
/// Releases the cookie when all characters are saved.
/// If all the conditions are met, it stops the core loop.
void chrif_check_shutdown(void) {
	if( runflag != MAPSERVER_ST_SHUTDOWN )
		return;
	if( auth_db->size(auth_db) > 0 )
		return;
	runflag = CORE_ST_STOP;
}

struct auth_node *chrif_search(int account_id) {
	return (struct auth_node *)idb_get(auth_db, account_id);
}

struct auth_node *chrif_auth_check(int account_id, int char_id, enum sd_state state) {
	struct auth_node *node = chrif_search(account_id);

	return (node && node->char_id == char_id && node->state == state) ? node : NULL;
}

bool chrif_auth_delete(int account_id, int char_id, enum sd_state state) {
	struct auth_node *node;

	if( (node = chrif_auth_check(account_id, char_id, state) ) ) {
		int fd = node->sd ? node->sd->fd : node->fd;

		if( session[fd] && session[fd]->session_data == node->sd )
			session[fd]->session_data = NULL;

		if( node->char_dat )
			aFree(node->char_dat);

		if( node->sd )
			aFree(node->sd);

		ers_free(auth_db_ers, node);
		idb_remove(auth_db,account_id);

		return true;
	}
	return false;
}

//Moves the sd character to the auth_db structure
static bool chrif_sd_to_auth(TBL_PC *sd, enum sd_state state) {
	struct auth_node *node;

	if( chrif_search(sd->status.account_id) )
		return false; //Already exists?

	node = ers_alloc(auth_db_ers, struct auth_node);

	memset(node, 0, sizeof(struct auth_node));

	node->account_id = sd->status.account_id;
	node->char_id = sd->status.char_id;
	node->login_id1 = sd->login_id1;
	node->login_id2 = sd->login_id2;
	node->sex = sd->status.sex;
	node->fd = sd->fd;
	node->sd = sd; //Data from logged on char
	node->node_created = gettick(); //timestamp for node timeouts
	node->state = state;

	sd->state.active = 0;
	idb_put(auth_db, node->account_id, node);
	return true;
}

static bool chrif_auth_logout(TBL_PC *sd, enum sd_state state) {

	if( sd->fd && state == ST_LOGOUT ) { //Disassociate player, and free it after saving ack returns [Skotlex]
		//fd info must not be lost for ST_MAPCHANGE as a final packet needs to be sent to the player
		if( session[sd->fd] )
			session[sd->fd]->session_data = NULL;
		sd->fd = 0;
	}

	return chrif_sd_to_auth(sd, state);
}

bool chrif_auth_finished(TBL_PC *sd) {
	struct auth_node *node = chrif_search(sd->status.account_id);

	if( node && node->sd == sd && node->state == ST_LOGIN ) {
		node->sd = NULL;

		return chrif_auth_delete(node->account_id, node->char_id, ST_LOGIN);
	}

	return false;
}
//Sets char-server's user id
void chrif_setuserid(char *id) {
	memcpy(userid, id, NAME_LENGTH);
}

//Sets char-server's password
void chrif_setpasswd(char *pwd) {
	memcpy(passwd, pwd, NAME_LENGTH);
}

//Security check, prints warning if using default password
void chrif_checkdefaultlogin(void) {
	//Skip this check if the server is run with run-once flag
	if (runflag != CORE_ST_STOP && !strcmp(userid, "s1") && !strcmp(passwd, "p1")) {
		ShowWarning("Using the default user/password s1/p1 is NOT RECOMMENDED.\n");
		ShowNotice("Please edit your 'login' table to create a proper inter-server user/password (gender 'S')\n");
		ShowNotice("and then edit your user/password in conf/map_athena.conf (or conf/import/map_conf.txt)\n");
	}
}

//Sets char-server's ip address
int chrif_setip(const char *ip) {
	char ip_str[16];

	if (!(char_ip = host2ip(ip))) {
		ShowWarning("Failed to Resolve Char Server Address! (%s)\n", ip);
		
		return 0;
	}

	safestrncpy(char_ip_str, ip, sizeof(char_ip_str));

	ShowInfo("Char Server IP Address : '"CL_WHITE"%s"CL_RESET"' -> '"CL_WHITE"%s"CL_RESET"'.\n", ip, ip2str(char_ip, ip_str));

	return 1;
}

//Sets char-server's port number
void chrif_setport(uint16 port) {
	char_port = port;
}

//Says whether the char-server is connected or not
int chrif_isconnected(void) {
	return (char_fd > 0 && session[char_fd] != NULL && chrif_state == 2);
}

/**
 * Saves character data.
 * @param sd: Player data
 * @param flag: Save flag types:
 *  CSAVE_NORMAL: Normal save
 *  CSAVE_QUIT: Character is quitting
 *  CSAVE_CHANGE_MAPSERV: Character is changing map-servers
 *  CSAVE_AUTOTRADE: Character used @autotrade
 *  CSAVE_INVENTORY: Character changed inventory data
 *  CSAVE_CART: Character changed cart data
 */
int chrif_save(struct map_session_data *sd, enum e_chrif_save_opt flag) {
	uint32 mmo_charstatus_len = 0;

	nullpo_retr(-1, sd);

	pc_makesavestatus(sd);

	if ((flag&CSAVE_QUITTING) && sd->state.active) { //Store player data which is quitting
		if (chrif_isconnected()) {
			chrif_save_scdata(sd);
			chrif_skillcooldown_save(sd);
		}
		if (!(flag&CSAVE_AUTOTRADE) && !chrif_auth_logout(sd, ((flag&CSAVE_QUIT) ? ST_LOGOUT : ST_MAPCHANGE)))
			ShowError("chrif_save: Failed to set up player %d:%d for proper quitting!\n", sd->status.account_id, sd->status.char_id);
	}

	chrif_check(-1); //Character is saved on reconnect

	chrif_bsdata_save(sd, ((flag&CSAVE_QUITTING) && !(flag&CSAVE_AUTOTRADE)));

	if (&sd->storage && sd->storage.dirty)
		storage_storagesave(sd);
	if (flag&CSAVE_INVENTORY)
		intif_storage_save(sd, &sd->inventory);
	if (flag&CSAVE_CART)
		intif_storage_save(sd, &sd->cart);

	//For data sync
	if (sd->state.storage_flag == 2)
		storage_guild_storagesave(sd->status.account_id, sd->status.guild_id, flag);
	if (&sd->premiumStorage && sd->premiumStorage.dirty)
		storage_premiumStorage_save(sd);

	if (flag&CSAVE_QUITTING)
		sd->state.storage_flag = 0; //Force close it

	//Saving of registry values.
	if (sd->state.reg_dirty&4)
		intif_saveregistry(sd, 3); //Save char regs
	if (sd->state.reg_dirty&2)
		intif_saveregistry(sd, 2); //Save account regs
	if (sd->state.reg_dirty&1)
		intif_saveregistry(sd, 1); //Save account2 regs

	mmo_charstatus_len = sizeof(sd->status) + 13;
	WFIFOHEAD(char_fd,mmo_charstatus_len);
	WFIFOW(char_fd,0) = 0x2b01;
	WFIFOW(char_fd,2) = mmo_charstatus_len;
	WFIFOL(char_fd,4) = sd->status.account_id;
	WFIFOL(char_fd,8) = sd->status.char_id;
	WFIFOB(char_fd,12) = (flag&CSAVE_QUIT) ? 1 : 0; //Flag to tell char-server this character is quitting.

	//If the user is on a instance map, we have to fake his current position
	if (map[sd->bl.m].instance_id) {
		struct mmo_charstatus status;

		//Copy the whole status
		memcpy(&status, &sd->status, sizeof(struct mmo_charstatus));
		//Change his current position to his savepoint
		memcpy(&status.last_point, &status.save_point, sizeof(struct point));
		//Copy the copied status into the packet
		memcpy(WFIFOP(char_fd, 13), &status, sizeof(struct mmo_charstatus));
	} else //Copy the whole status into the packet
		memcpy(WFIFOP(char_fd, 13), &sd->status, sizeof(struct mmo_charstatus));

	WFIFOSET(char_fd, WFIFOW(char_fd,2));

	if (sd->status.pet_id > 0 && sd->pd)
		intif_save_petdata(sd->status.account_id, &sd->pd->pet);
	if (hom_is_active(sd->hd))
		hom_save(sd->hd);
	if (sd->md && mercenary_get_lifetime(sd->md) > 0)
		mercenary_save(sd->md);
	if (sd->ed && elemental_get_lifetime(sd->ed) > 0)
		elemental_save(sd->ed);
	if (sd->save_quest)
		intif_quest_save(sd);

	return 0;
}

//Connects to char-server (plaintext)
/**
 * Map-serv request to login into char-serv
 * @param fd : char-serv fd to log into
 * @return 0:request sent
 */
int chrif_connect(int fd) {
	ShowStatus("Logging in to char server...\n", char_fd);
	WFIFOHEAD(fd,60);
	WFIFOW(fd,0) = 0x2af8;
	memcpy(WFIFOP(fd,2), userid, NAME_LENGTH);
	memcpy(WFIFOP(fd,26), passwd, NAME_LENGTH);
	WFIFOL(fd,50) = 0;
	WFIFOL(fd,54) = htonl(clif_getip());
	WFIFOW(fd,58) = htons(clif_getport());
	WFIFOSET(fd,60);

	return 0;
}

//Sends maps to char-server
int chrif_sendmap(int fd) {
	int i;

	ShowStatus("Sending maps to char server...\n");

	//Sending normal maps, not instances
	WFIFOHEAD(fd, 4 + instance_start * 4);
	WFIFOW(fd,0) = 0x2afa;
	for(i = 0; i < instance_start; i++)
		WFIFOW(fd,4 + i * 4) = map_id2index(i);
	WFIFOW(fd,2) = 4 + i * 4;
	WFIFOSET(fd,WFIFOW(fd,2));

	return 0;
}

//Receive maps from some other map-server (relayed via char-server)
int chrif_recvmap(int fd) {
	int i, j;
	uint32 ip = ntohl(RFIFOL(fd,4));
	uint16 port = ntohs(RFIFOW(fd,8));

	for(i = 10, j = 0; i < RFIFOW(fd,2); i += 4, j++) {
		map_setipport(RFIFOW(fd,i), ip, port);
	}

	if (battle_config.etc_log)
		ShowStatus("Received maps from %d.%d.%d.%d:%d (%d maps)\n", CONVIP(ip), port, j);

	other_mapserver_count++;

	return 0;
}

//Remove specified maps (used when some other map-server disconnects)
int chrif_removemap(int fd) {
	int i, j;
	uint32 ip =  RFIFOL(fd,4);
	uint16 port = RFIFOW(fd,8);

	for(i = 10, j = 0; i < RFIFOW(fd, 2); i += 4, j++)
		map_eraseipport(RFIFOW(fd, i), ip, port);

	other_mapserver_count--;

	if(battle_config.etc_log)
		ShowStatus("remove map of server %d.%d.%d.%d:%d (%d maps)\n", CONVIP(ip), port, j);

	return 0;
}

//Received after a character has been "final saved" on the char-server
static void chrif_save_ack(int fd) {
	chrif_auth_delete(RFIFOL(fd,2), RFIFOL(fd,6), ST_LOGOUT);
	chrif_check_shutdown();
}

//Request to move a character between mapservers
int chrif_changemapserver(struct map_session_data *sd, uint32 ip, uint16 port) {
	nullpo_retr(-1, sd);

	if (other_mapserver_count < 1) { //No other map servers are online!
		clif_authfail_fd(sd->fd, 0);
		return -1;
	}

	chrif_check(-1);

	WFIFOHEAD(char_fd,35);
	WFIFOW(char_fd, 0) = 0x2b05;
	WFIFOL(char_fd, 2) = sd->bl.id;
	WFIFOL(char_fd, 6) = sd->login_id1;
	WFIFOL(char_fd,10) = sd->login_id2;
	WFIFOL(char_fd,14) = sd->status.char_id;
	WFIFOW(char_fd,18) = sd->mapindex;
	WFIFOW(char_fd,20) = sd->bl.x;
	WFIFOW(char_fd,22) = sd->bl.y;
	WFIFOL(char_fd,24) = htonl(ip);
	WFIFOW(char_fd,28) = htons(port);
	WFIFOB(char_fd,30) = sd->status.sex;
	WFIFOL(char_fd,31) = htonl(session[sd->fd]->client_addr);
	WFIFOL(char_fd,35) = sd->group_id;
	WFIFOSET(char_fd,39);

	return 0;
}

/// map-server change (mapserv) request acknowledgement (positive or negative)
/// R 2b06 <account_id>.L <login_id1>.L <login_id2>.L <char_id>.L <map_index>.W <x>.W <y>.W <ip>.L <port>.W
int chrif_changemapserverack(int account_id, int login_id1, int login_id2, int char_id, short map_index, short x, short y, uint32 ip, uint16 port) {
	struct auth_node *node;

	if ( !(node = chrif_auth_check(account_id, char_id, ST_MAPCHANGE)) )
		return -1;

	if ( !login_id1 ) {
		ShowError("map server change failed.\n");
		clif_authfail_fd(node->fd, 0);
	} else
		clif_changemapserver(node->sd, map_index, x, y, ntohl(ip), ntohs(port));

	//Player has been saved already, remove him from memory [Skotlex]
	chrif_auth_delete(account_id, char_id, ST_MAPCHANGE);

	return 0;
}

/**
 * Does the char_serv have validate our connection to him ?
 * If yes then 
 *  - Send all our mapname to charserv
 *  - Retrieve guild castle
 *  - Do OnInterIfInit and OnInterIfInitOnce on all npc 
 * 0x2af9 <errCode>B
 */
int chrif_connectack(int fd) {
	if (RFIFOB(fd,2)) {
		ShowFatalError("Connection to char-server failed %d.\n", RFIFOB(fd,2));
		exit(EXIT_FAILURE);
	}

	ShowStatus("Successfully logged on to Char Server (Connection: '"CL_WHITE"%d"CL_RESET"').\n",fd);
	chrif_state = 1;
	chrif_connected = 1;

	chrif_sendmap(fd);

	ShowStatus("Event '"CL_WHITE"OnInterIfInit"CL_RESET"' executed with '"CL_WHITE"%d"CL_RESET"' NPCs.\n", npc_event_doall("OnInterIfInit"));
	if( !char_init_done ) {
		ShowStatus("Event '"CL_WHITE"OnInterIfInitOnce"CL_RESET"' executed with '"CL_WHITE"%d"CL_RESET"' NPCs.\n", npc_event_doall("OnInterIfInitOnce"));
		guild_castle_map_init();
		intif_clan_requestclans();
	}

	socket_datasync(fd, true); 

	return 0;
}

/**
 * @see DBApply
 */
static int chrif_reconnect(DBKey key, DBData *data, va_list ap) {
	struct auth_node *node = db_data2ptr(data);

	switch (node->state) {
		case ST_LOGIN:
			if ( node->sd && !node->char_dat ) { //Since there is no way to request the char auth, make it fail
				pc_authfail(node->sd);
				chrif_char_offline(node->sd);
				chrif_auth_delete(node->account_id, node->char_id, ST_LOGIN);
			}
			break;
		case ST_LOGOUT:
			chrif_save(node->sd, CSAVE_QUIT|CSAVE_INVENTORY|CSAVE_CART); //Re-send final save
			break;
		case ST_MAPCHANGE: { //Re-send map-change request
				struct map_session_data *sd = node->sd;
				uint32 ip;
				uint16 port;

				if( !map_mapname2ipport(sd->mapindex,&ip,&port) )
					chrif_changemapserver(sd, ip, port);
				else //Too much lag/timeout is the closest explanation for this error
					clif_authfail_fd(sd->fd, 3);
			}
			break;
	}

	return 0;
}


/// Called when all the connection steps are completed.
void chrif_on_ready(void) {
	ShowStatus("Map Server is now online.\n");

	chrif_state = 2;

	chrif_check_shutdown();

	//If there are players online, send them to the char-server [Skotlex]
	send_users_tochar();

	//Auth db reconnect handling
	auth_db->foreach(auth_db,chrif_reconnect);

	//Re-save any storages that were modified in the disconnection time [Skotlex]
	do_reconnect_storage();

	//Re-save any guild castles that were modified in the disconnection time
	guild_castle_reconnect(-1, 0, 0);

	//Charserver is ready for loading autotrader
	if( !char_init_done ) {
		do_init_buyingstore_autotrade();
		do_init_vending_autotrade();
		char_init_done = true;
	}
}


/**
 * Maps are sent, then received misc info from char-server
 * - Server name
 * - Default map
 * HZ 0x2afb
 */
int chrif_sendmapack(int fd) {
	uint16 offs = 5;

	if (RFIFOB(fd,4)) {
		ShowFatalError("chrif : send map list to char server failed %d\n", RFIFOB(fd,2));
		exit(EXIT_FAILURE);
	}

	//Server name
	memcpy(wisp_server_name, RFIFOP(fd,5), NAME_LENGTH);
	ShowStatus("Map-server connected to char-server '"CL_WHITE"%s"CL_RESET"'.\n", wisp_server_name);

	//Default map
	memcpy(map_default.mapname, RFIFOP(fd,(offs += NAME_LENGTH)), MAP_NAME_LENGTH);
	map_default.x = RFIFOW(fd,(offs += MAP_NAME_LENGTH));
	map_default.y = RFIFOW(fd,(offs += 2));
	if (battle_config.etc_log)
		ShowInfo("Received default map from char-server '"CL_WHITE"%s %d,%d"CL_RESET"'.\n", map_default.mapname, map_default.x, map_default.y);

	chrif_on_ready();

	return 0;
}

/*==========================================
 * Request sc_data from charserver [Skotlex]
 *------------------------------------------*/
int chrif_scdata_request(int account_id, int char_id) {
#ifdef ENABLE_SC_SAVING
	chrif_check(-1);

	WFIFOHEAD(char_fd,10);
	WFIFOW(char_fd,0) = 0x2afc;
	WFIFOL(char_fd,2) = account_id;
	WFIFOL(char_fd,6) = char_id;
	WFIFOSET(char_fd,10);
#endif
	return 0;
}

/*==========================================
 * Request skillcooldown from charserver
 *------------------------------------------*/
int chrif_skillcooldown_request(int account_id, int char_id) {
	chrif_check(-1);
	WFIFOHEAD(char_fd,10);
	WFIFOW(char_fd,0) = 0x2b0a;
	WFIFOL(char_fd,2) = account_id;
	WFIFOL(char_fd,6) = char_id;
	WFIFOSET(char_fd,10);
	return 0;
}

/*==========================================
 * Request auth confirmation
 *------------------------------------------*/
void chrif_authreq(struct map_session_data *sd, bool autotrade) {
	struct auth_node *node = chrif_search(sd->bl.id);

	if( node != NULL || !chrif_isconnected() ) {
		set_eof(sd->fd);
		return;
	}

	WFIFOHEAD(char_fd,20);
	WFIFOW(char_fd,0) = 0x2b26;
	WFIFOL(char_fd,2) = sd->status.account_id;
	WFIFOL(char_fd,6) = sd->status.char_id;
	WFIFOL(char_fd,10) = sd->login_id1;
	WFIFOB(char_fd,14) = sd->status.sex;
	WFIFOL(char_fd,15) = htonl(session[sd->fd]->client_addr);
	WFIFOB(char_fd,19) = autotrade;
	WFIFOSET(char_fd,20);
	chrif_sd_to_auth(sd, ST_LOGIN);
}

/*==========================================
 * Auth confirmation ack
 *------------------------------------------*/
void chrif_authok(int fd) {
	int account_id, group_id, char_id;
	uint32 login_id1, login_id2;
	time_t expiration_time;
	struct mmo_charstatus *status;
	struct auth_node *node;
	bool changing_mapservers;
	TBL_PC *sd;

	//Check if both servers agree on the struct's size
	if( RFIFOW(fd,2) - 25 != sizeof(struct mmo_charstatus) ) {
		ShowError("chrif_authok: Data size mismatch! %d != %d\n", RFIFOW(fd,2) - 25, sizeof(struct mmo_charstatus));
		return;
	}

	account_id = RFIFOL(fd,4);
	login_id1 = RFIFOL(fd,8);
	login_id2 = RFIFOL(fd,12);
	expiration_time = (time_t)(int32)RFIFOL(fd,16);
	group_id = RFIFOL(fd,20);
	changing_mapservers = (RFIFOB(fd,24));
	status = (struct mmo_charstatus *)RFIFOP(fd,25);
	char_id = status->char_id;

	//Check if we don't already have player data in our server
	//Causes problems if the currently connected player tries to quit or this data belongs to an already connected player which is trying to re-auth.
	if( (sd = map_id2sd(account_id)) )
		return;

	if( !(node = chrif_search(account_id)) )
		return; //Should not happen

	if( node->state != ST_LOGIN )
		return; //Character in logout phase, do not touch that data.

	if( !node->sd ) {
		/*
		//When we receive double login info and the client has not connected yet,
		//discard the older one and keep the new one.
		chrif_auth_delete(node->account_id, node->char_id, ST_LOGIN);
		*/
		return; //Should not happen
	}

	sd = node->sd;

	if( runflag == MAPSERVER_ST_RUNNING &&
		!node->char_dat &&
		node->account_id == account_id &&
		node->char_id == char_id &&
		node->login_id1 == login_id1 )
	{ //Auth Ok
		if( pc_authok(sd, login_id2, expiration_time, group_id, status, changing_mapservers) )
			return;
	} else //Auth Failed
		pc_authfail(sd);

	chrif_char_offline(sd); //Set him offline, the char server likely has it set as online already.
	chrif_auth_delete(account_id, char_id, ST_LOGIN);
}

// Client authentication failed
void chrif_authfail(int fd) { /* HELLO WORLD. ip in RFIFOL 15 is not being used (but is available) */
	int account_id, char_id;
	uint32 login_id1;
	char sex;
	struct auth_node *node;

	account_id = RFIFOL(fd,2);
	char_id    = RFIFOL(fd,6);
	login_id1  = RFIFOL(fd,10);
	sex        = RFIFOB(fd,14);

	node = chrif_search(account_id);

	if( node != NULL &&
		node->account_id == account_id &&
		node->char_id == char_id &&
		node->login_id1 == login_id1 &&
		node->sex == sex &&
		node->state == ST_LOGIN )
	{ // Found a match
		clif_authfail_fd(node->fd, 0);
		chrif_auth_delete(account_id, char_id, ST_LOGIN);
	}
}


/**
 * This can still happen (client times out while waiting for char to confirm auth data)
 * @see DBApply
 */
int auth_db_cleanup_sub(DBKey key, DBData *data, va_list ap) {
	struct auth_node *node = db_data2ptr(data);

	if( DIFF_TICK(gettick(), node->node_created) > 60000 ) {
		const char *states[] = { "Login", "Logout", "Map change" };

		switch( node->state ) {
			case ST_LOGOUT:
				//Re-save attempt (->sd should never be null here).
				node->node_created = gettick(); //Refresh tick (avoid char-server load if connection is really bad)
				chrif_save(node->sd, CSAVE_QUIT|CSAVE_INVENTORY|CSAVE_CART);
				break;
			default:
				//Clear data. any connected players should have timed out by now.
				ShowInfo("auth_db: Node (state %s) timed out for %d:%d\n", states[node->state], node->account_id, node->char_id);
				chrif_char_offline_nsd(node->account_id, node->char_id);
				chrif_auth_delete(node->account_id, node->char_id, node->state);
				break;
		}
		return 1;
	}
	return 0;
}

int auth_db_cleanup(int tid, unsigned int tick, int id, intptr_t data) {
	chrif_check(0);
	auth_db->foreach(auth_db, auth_db_cleanup_sub);
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
int chrif_charselectreq(struct map_session_data *sd, uint32 s_ip) {
	nullpo_retr(-1, sd);

	if( !sd || !sd->bl.id || !sd->login_id1 )
		return -1;

	chrif_check(-1);

	WFIFOHEAD(char_fd,22);
	WFIFOW(char_fd,0) = 0x2b02;
	WFIFOL(char_fd,2) = sd->bl.id;
	WFIFOL(char_fd,6) = sd->login_id1;
	WFIFOL(char_fd,10) = sd->login_id2;
	WFIFOL(char_fd,14) = htonl(s_ip);
	WFIFOB(char_fd,18) = sd->packet_ver;
	WFIFOL(char_fd,19) = sd->group_id;
	WFIFOSET(char_fd,22);

	return 0;
}

/*==========================================
 * Search Char trough id on char server
 *------------------------------------------*/
int chrif_searchcharid(int char_id) {

	if( !char_id )
		return -1;

	chrif_check(-1);

	WFIFOHEAD(char_fd,6);
	WFIFOW(char_fd,0) = 0x2b08;
	WFIFOL(char_fd,2) = char_id;
	WFIFOSET(char_fd,6);

	return 0;
}

/*==========================================
 * Change Email
 *------------------------------------------*/
int chrif_changeemail(int id, const char *actual_email, const char *new_email) {

	if (battle_config.etc_log)
		ShowInfo("chrif_changeemail: account: %d, actual_email: '%s', new_email: '%s'.\n", id, actual_email, new_email);

	chrif_check(-1);

	WFIFOHEAD(char_fd,86);
	WFIFOW(char_fd,0) = 0x2b0c;
	WFIFOL(char_fd,2) = id;
	memcpy(WFIFOP(char_fd,6), actual_email, 40);
	memcpy(WFIFOP(char_fd,46), new_email, 40);
	WFIFOSET(char_fd,86);

	return 0;
}

/**
 * S 2b0e <accid>.l <name>.24B <operation_type>.w <timediff>L <val1>L <val2>L
 * Send an account modification request to the login server (via char server).
 * @aid : Player requesting operation account id
 * @character_name : Target of operation Player name
 * @operation_type : see chrif_req_op
 * @timediff : tick to add or remove to unixtimestamp
 * @val1 : extra data value to transfer for operation
 * CHRIF_OP_LOGIN_VIP: 0x1 : Select info and update old_groupid
 *                     0x2 : VIP duration is changed by atcommand or script
 *                     0x4 : Show status reply by char-server through 0x2b0f
 *                     0x8 : First request on player login
 */
int chrif_req_login_operation(int aid, const char *character_name, enum chrif_req_op operation_type, int32 timediff, int val1) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,40);
	WFIFOW(char_fd,0) = 0x2b0e;
	WFIFOL(char_fd,2) = aid;
	safestrncpy((char *)WFIFOP(char_fd,6), character_name, NAME_LENGTH);
	WFIFOW(char_fd,30) = operation_type;
	if (operation_type == CHRIF_OP_LOGIN_BAN || operation_type == CHRIF_OP_LOGIN_VIP)
		WFIFOL(char_fd,32) = timediff;
	WFIFOL(char_fd,36) = val1;
	WFIFOSET(char_fd,40);

	return 0;
}

/**
 * S 2b0e <accid>.l <name>.24B <operation_type>.w <timediff>L <val1>L <val2>L
 * Send a sex change (for account or character) request to the login server (via char server).
 * @sd : Player requesting operation
 */
int chrif_changesex(struct map_session_data *sd, bool change_account) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,40);
	WFIFOW(char_fd,0) = 0x2b0e;
	WFIFOL(char_fd,2) = sd->status.account_id;
	safestrncpy((char *)WFIFOP(char_fd,6), sd->status.name, NAME_LENGTH);
	WFIFOW(char_fd,30) = (change_account ? CHRIF_OP_LOGIN_CHANGESEX : CHRIF_OP_CHANGECHARSEX);
	if (!change_account)
		WFIFOB(char_fd,32) = (sd->status.sex == SEX_MALE ? SEX_FEMALE : SEX_MALE);
	WFIFOSET(char_fd,40);
	clif_displaymessage(sd->fd, msg_txt(408)); // "Need disconnection to perform change-sex request..."
	if (sd->fd)
		clif_authfail_fd(sd->fd, 15);
	else
		map_quit(sd);

	return 0;
}

/**
 * R 2b0f <accid>.l <name>.24B <type>.w <answer>.w
 * Processing a reply to chrif_req_login_operation() (request to modify an account).
 * NB: That ack is received just after the char has sent the request to login and therefore didn't have login reply yet
 * @param aid : player account id the request was concerning
 * @param player_name : name the request was concerning
 * @param type : code of operation done. See enum chrif_req_op
 * @param awnser : type of anwser
 *   0: login-server request done
 *   1: player not found
 *   2: gm level too low
 *   3: login-server offline
 */
static void chrif_ack_login_req(int aid, const char *player_name, uint16 type, uint16 answer) {
	struct map_session_data *sd;
	char action[25];
	char output[256];

	sd = map_id2sd(aid);
	if (aid < 0 || sd == NULL) {
		ShowError("chrif_ack_login_req failed - player not online.\n");
		return;
	}

	switch (type) {
		case CHRIF_OP_LOGIN_CHANGESEX:
		case CHRIF_OP_CHANGECHARSEX:
			type = CHRIF_OP_LOGIN_CHANGESEX; //So we don't have to create a new msgstring
		//Fall through
		case CHRIF_OP_LOGIN_BLOCK:
		case CHRIF_OP_LOGIN_BAN:
		case CHRIF_OP_LOGIN_UNBLOCK:
		case CHRIF_OP_LOGIN_UNBAN:
			snprintf(action, 25, "%s", msg_txt(427 + type)); // Block|Ban|Unblock|Unban|Change the sex of
			break;
		case CHRIF_OP_LOGIN_VIP:
			if (!battle_config.disp_servervip_msg)
				return;
			snprintf(action, 25, "%s", msg_txt(436)); // VIP
			break;
		default:
			snprintf(action, 25, "???");
			break;
	}

	switch (answer) {
		case 0: sprintf(output, msg_txt(424), action, NAME_LENGTH, player_name); break; // Login-serv has been asked to %s the player '%.*s'.
		case 1: sprintf(output, msg_txt(425), NAME_LENGTH, player_name); break;
		case 2: sprintf(output, msg_txt(426), action, NAME_LENGTH, player_name); break;
		case 3: sprintf(output, msg_txt(427), action, NAME_LENGTH, player_name); break;
		case 4: sprintf(output, msg_txt(424), action, NAME_LENGTH, player_name); break;
		default: output[0] = '\0'; break;
	}

	clif_displaymessage(sd->fd, output);
}

/*==========================================
 * Request char server to change sex of char (modified by Yor)
 *------------------------------------------*/
void chrif_changedsex(int fd) {
	int acc = RFIFOL(fd,2);
	//int sex = RFIFOL(fd,6); //Dead store, uncomment if needed again

	if (battle_config.etc_log)
		ShowNotice("chrif_changedsex %d.\n", acc);
	//Path to activate this response:
	//Map(start) (0x2b0e) -> Char(0x2727) -> Login
	//Login(0x2723) [ALL] -> Char (0x2b0d)[ALL] -> Map (HERE)
	//OR
	//Map(start) (0x2b03) -> Char
	//Char(0x2b0d)[ALL] -> Map (HERE)
	//Char will usually be "logged in" despite being forced to log-out in the begining of this process [Panikon]
}

/*==========================================
 * Request Char Server to Divorce Players
 *------------------------------------------*/
int chrif_divorce(int partner_id1, int partner_id2) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,10);
	WFIFOW(char_fd,0) = 0x2b11;
	WFIFOL(char_fd,2) = partner_id1;
	WFIFOL(char_fd,6) = partner_id2;
	WFIFOSET(char_fd,10);

	return 0;
}

/*==========================================
 * Divorce players
 * only used if 'partner_id' is offline
 *------------------------------------------*/
int chrif_divorceack(int char_id, int partner_id) {
	struct map_session_data *sd;
	int i;

	if( !char_id || !partner_id )
		return 0;

	if( (sd = map_charid2sd(char_id)) && sd->status.partner_id == partner_id ) {
		sd->status.partner_id = 0;
		for( i = 0; i < MAX_INVENTORY; i++ )
			if( sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_M || sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_F )
				pc_delitem(sd, i, 1, 0, 0, LOG_TYPE_OTHER);
	}

	if( (sd = map_charid2sd(partner_id)) && sd->status.partner_id == char_id ) {
		sd->status.partner_id = 0;
		for( i = 0; i < MAX_INVENTORY; i++ )
			if( sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_M || sd->inventory.u.items_inventory[i].nameid == WEDDING_RING_F )
				pc_delitem(sd, i, 1, 0, 0, LOG_TYPE_OTHER);
	}

	return 0;
}
/*==========================================
 * Removes Baby from parents
 *------------------------------------------*/
int chrif_deadopt(int father_id, int mother_id, int child_id) {
	struct map_session_data *sd;
	uint16 idx = skill_get_index(WE_CALLBABY);

	if( father_id && (sd = map_charid2sd(father_id)) && sd->status.child == child_id ) {
		sd->status.child = 0;
		sd->status.skill[idx].id = 0;
		sd->status.skill[idx].lv = 0;
		sd->status.skill[idx].flag = SKILL_FLAG_PERMANENT;
		clif_deleteskill(sd,WE_CALLBABY);
	}

	if( mother_id && (sd = map_charid2sd(mother_id)) && sd->status.child == child_id ) {
		sd->status.child = 0;
		sd->status.skill[idx].id = 0;
		sd->status.skill[idx].lv = 0;
		sd->status.skill[idx].flag = SKILL_FLAG_PERMANENT;
		clif_deleteskill(sd,WE_CALLBABY);
	}

	return 0;
}

/*==========================================
 * Disconnection of a player (account has been banned of has a status, from login/char-server) by [Yor]
 *------------------------------------------*/
int chrif_ban(int fd) {
	int id, res = 0;
	struct map_session_data *sd;

	id = RFIFOL(fd,2);
	res = RFIFOB(fd,6); //0: change of statut, 1: ban, 2 charban

	if( battle_config.etc_log )
		ShowNotice("chrif_ban %d.type = %s \n", id, res == 1 ? "account" : "char");

	if( res == 2 )
		sd = map_charid2sd(id);
	else
		sd = map_id2sd(id);

	if( id < 0 || !sd )
		return 0; //Nothing to do on map if player not connected

	sd->login_id1++; //Change identify, because if player come back in char within the 5 seconds, he can change its characters
	if( !res ) {
		int ret_status = RFIFOL(fd,7); //Status or final date of a banishment

		if( 0 < ret_status && ret_status <= 9 )
			clif_displaymessage(sd->fd, msg_txt(411 + ret_status));
		else if( ret_status == 100 )
			clif_displaymessage(sd->fd, msg_txt(421));
		else
			clif_displaymessage(sd->fd, msg_txt(420)); // "Your account has not more authorized."
	} else if( res == 1 || res == 2 ) {
		time_t timestamp;
		char tmpstr[256];
		char strtime[25];

		timestamp = (time_t)RFIFOL(fd,7); //Status or final date of a banishment
		strftime(strtime, 24, "%d-%m-%Y %H:%M:%S", localtime(&timestamp));
		safesnprintf(tmpstr, sizeof(tmpstr), msg_txt(423), (res == 2 ? "char" : "account"), strtime); // "Your %s has been banished until %s "
		clif_displaymessage(sd->fd, tmpstr);
	}

	set_eof(sd->fd); //Forced to disconnect for the change
	map_quit(sd); //Remove leftovers (e.g. autotrading) [Paradox924X]
	return 0;
}

int chrif_req_charban(int aid, const char *character_name, int32 timediff) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,10 + NAME_LENGTH);
	WFIFOW(char_fd,0) = 0x2b28;
	WFIFOL(char_fd,2) = aid;
	WFIFOL(char_fd,6) = timediff;
	safestrncpy((char *)WFIFOP(char_fd,10), character_name, NAME_LENGTH);
	WFIFOSET(char_fd,10 + NAME_LENGTH); //Default 34
	return 0;
}

int chrif_req_charunban(int aid, const char *character_name) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,6 + NAME_LENGTH);
	WFIFOW(char_fd,0) = 0x2b2a;
	WFIFOL(char_fd,2) = aid;
	safestrncpy((char *)WFIFOP(char_fd,6), character_name, NAME_LENGTH);
	WFIFOSET(char_fd,6 + NAME_LENGTH);
	return 0;
}

//Disconnect the player out of the game, simple packet
//packet.w AID.L WHY.B 2+4+1 = 7byte
int chrif_disconnectplayer(int fd) {
	struct map_session_data *sd;
	int account_id = RFIFOL(fd, 2);

	if( !(sd = map_id2sd(account_id)) ) {
		struct auth_node *auth = chrif_search(account_id);

		if( auth && chrif_auth_delete(account_id, auth->char_id, ST_LOGIN) )
			return 0;

		return -1;
	}

	if( !sd->fd ) { //No connection
		if( sd->state.autotrade )
			map_quit(sd); //Remove it
		//Else we don't remove it because the char should have a timer to remove the player because it force-quit before,
		//and we don't want them kicking their previous instance before the 10 secs penalty time passes [Skotlex]
		return 0;
	}

	switch( RFIFOB(fd,6) ) {
		case 1: clif_authfail_fd(sd->fd, 1); break; //Server closed
		case 2: clif_authfail_fd(sd->fd, 2); break; //Someone else logged in
		case 3: clif_authfail_fd(sd->fd, 4); break; //Server overpopulated
		case 4: clif_authfail_fd(sd->fd, 10); break; //Out of available time paid for
		case 5: clif_authfail_fd(sd->fd, 15); break; //Forced to dc by gm
	}
	return 0;
}

/*==========================================
 * Request/Receive top 10 Fame character list
 *------------------------------------------*/
int chrif_updatefamelist(struct map_session_data *sd) {
	char type;

	chrif_check(-1);

	switch(sd->class_&MAPID_UPPERMASK) {
		case MAPID_BLACKSMITH: type = RANK_BLACKSMITH; break;
		case MAPID_ALCHEMIST: type = RANK_ALCHEMIST; break;
		case MAPID_TAEKWON: type = RANK_TAEKWON; break;
		default:
			return 0;
	}

	WFIFOHEAD(char_fd, 11);
	WFIFOW(char_fd,0) = 0x2b10;
	WFIFOL(char_fd,2) = sd->status.char_id;
	WFIFOL(char_fd,6) = sd->status.fame;
	WFIFOB(char_fd,10) = type;
	WFIFOSET(char_fd,11);

	return 0;
}

int chrif_buildfamelist(void) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,2);
	WFIFOW(char_fd,0) = 0x2b1a;
	WFIFOSET(char_fd,2);

	return 0;
}

int chrif_recvfamelist(int fd) {
	int num, size;
	int total = 0, len = 8;

	memset (smith_fame_list, 0, sizeof(smith_fame_list));
	memset (chemist_fame_list, 0, sizeof(chemist_fame_list));
	memset (taekwon_fame_list, 0, sizeof(taekwon_fame_list));

	size = RFIFOW(fd, 6); //Blacksmith block size

	for (num = 0; len < size && num < MAX_FAME_LIST; num++) {
		memcpy(&smith_fame_list[num], RFIFOP(fd,len), sizeof(struct fame_list));
 		len += sizeof(struct fame_list);
	}

	total += num;

	size = RFIFOW(fd, 4); //Alchemist block size

	for (num = 0; len < size && num < MAX_FAME_LIST; num++) {
		memcpy(&chemist_fame_list[num], RFIFOP(fd,len), sizeof(struct fame_list));
 		len += sizeof(struct fame_list);
	}

	total += num;

	size = RFIFOW(fd, 2); //Total packet length

	for (num = 0; len < size && num < MAX_FAME_LIST; num++) {
		memcpy(&taekwon_fame_list[num], RFIFOP(fd,len), sizeof(struct fame_list));
 		len += sizeof(struct fame_list);
	}

	total += num;

	ShowInfo("Received Fame List of '"CL_WHITE"%d"CL_RESET"' characters.\n", total);

	return 0;
}

/// Fame ranking update confirmation
/// R 2b22 <table>.B <index>.B <value>.L
int chrif_updatefamelist_ack(int fd) {
	struct fame_list* list;
	uint8 index;

	switch (RFIFOB(fd,2)) {
		case RANK_BLACKSMITH: list = smith_fame_list; break;
		case RANK_ALCHEMIST: list = chemist_fame_list; break;
		case RANK_TAEKWON: list = taekwon_fame_list; break;
		default:
			return 0;
	}

	index = RFIFOB(fd,3);

	if (index >= MAX_FAME_LIST)
		return 0;

	list[index].fame = RFIFOL(fd,4);

	return 1;
}

//Parses the sc_data of the player and sends it to the char-server for saving [Skotlex]
int chrif_save_scdata(struct map_session_data *sd) {
#ifdef ENABLE_SC_SAVING
	int i, count = 0;
	unsigned int tick;
	struct status_change_data data;
	struct status_change *sc = &sd->sc;
	const struct TimerData *timer;

	chrif_check(-1);
	tick = gettick();

	WFIFOHEAD(char_fd,14 + SC_MAX * sizeof(struct status_change_data));
	WFIFOW(char_fd,0) = 0x2b1c;
	WFIFOL(char_fd,4) = sd->status.account_id;
	WFIFOL(char_fd,8) = sd->status.char_id;

	for (i = 0; i < SC_MAX; i++) {
		if (!sc->data[i])
			continue;
		if (sc->data[i]->timer != INVALID_TIMER) {
			timer = get_timer(sc->data[i]->timer);
			if (!timer || timer->func != status_change_timer)
				continue;
			if (DIFF_TICK(timer->tick,tick) > 0)
				data.tick = DIFF_TICK(timer->tick,tick); //Duration that is left before ending
			else
				data.tick = 0; //Negative tick does not necessarily mean that sc has expired
		} else
			data.tick = INVALID_TIMER; //Infinite duration
		data.type = i;
		data.val1 = sc->data[i]->val1;
		data.val2 = sc->data[i]->val2;
		data.val3 = sc->data[i]->val3;
		data.val4 = sc->data[i]->val4;
		memcpy(WFIFOP(char_fd,14 + count * sizeof(struct status_change_data)), &data, sizeof(struct status_change_data));
		count++;
	}

	WFIFOW(char_fd,12) = count;
	WFIFOW(char_fd,2) = 14 + count * sizeof(struct status_change_data); //Total packet size
	WFIFOSET(char_fd,WFIFOW(char_fd,2));
#endif

	return 0;
}

int chrif_skillcooldown_save(struct map_session_data *sd) {
	int i, count = 0;
	struct skill_cooldown_data data;
	unsigned int tick;
	const struct TimerData *timer;

	chrif_check(-1);
	tick = gettick();

	WFIFOHEAD(char_fd,14 + MAX_SKILLCOOLDOWN * sizeof (struct skill_cooldown_data));
	WFIFOW(char_fd,0) = 0x2b15;
	WFIFOL(char_fd,4) = sd->status.account_id;
	WFIFOL(char_fd,8) = sd->status.char_id;

	for (i = 0; i < MAX_SKILLCOOLDOWN; i++) {
		if (!sd->scd[i])
			continue;
		if (!battle_config.guild_skill_relog_delay && (sd->scd[i]->skill_id >= GD_BATTLEORDER && sd->scd[i]->skill_id <= GD_EMERGENCYCALL))
			continue;
		timer = get_timer(sd->scd[i]->timer);
		if (!timer || timer->func != skill_blockpc_end || DIFF_TICK(timer->tick, tick) < 0)
			continue;
		data.tick = DIFF_TICK(timer->tick, tick);
		data.duration = sd->scd[i]->duration;
		data.skill_id = sd->scd[i]->skill_id;
		memcpy(WFIFOP(char_fd,14 + count * sizeof (struct skill_cooldown_data)), &data, sizeof (struct skill_cooldown_data));
		count++;
	}

	WFIFOW(char_fd,12) = count;
	WFIFOW(char_fd,2) = 14 + count * sizeof (struct skill_cooldown_data);
	WFIFOSET(char_fd,WFIFOW(char_fd,2));

	return 0;
}

//Retrieve and load sc_data for a player [Skotlex]
int chrif_load_scdata(int fd) {
#ifdef ENABLE_SC_SAVING
	struct map_session_data *sd;
	int aid, cid, i, count;

	aid = RFIFOL(fd,4); //Player Account ID
	cid = RFIFOL(fd,8); //Player Char ID

	sd = map_id2sd(aid);

	if (!sd) {
		ShowError("chrif_load_scdata: Player of AID %d not found!\n", aid);
		return -1;
	}

	if (sd->status.char_id != cid) {
		ShowError("chrif_load_scdata: Receiving data for account %d, char id does not matches (%d != %d)!\n", aid, sd->status.char_id, cid);
		return -1;
	}

	count = RFIFOW(fd,12); //sc count

	for (i = 0; i < count; i++) {
		struct status_change_data *data = (struct status_change_data *)RFIFOP(fd,14 + i * sizeof(struct status_change_data));

		status_change_start(NULL, &sd->bl, (sc_type)data->type, 10000,
			data->val1, data->val2, data->val3, data->val4, data->tick, SCFLAG_NOAVOID|SCFLAG_FIXEDTICK|SCFLAG_LOADED|SCFLAG_FIXEDRATE);
	}

	pc_scdata_received(sd);
#endif

	return 0;
}

//Retrieve and load skillcooldown for a player
int chrif_skillcooldown_load(int fd) {
	struct map_session_data *sd;
	int aid, cid, i, count;

	aid = RFIFOL(fd,4);
	cid = RFIFOL(fd,8);

	sd = map_id2sd(aid);

	if (!sd) {
		ShowError("chrif_skillcooldown_load: Player of AID %d not found!\n", aid);
		return -1;
	}

	if (sd->status.char_id != cid) {
		ShowError("chrif_skillcooldown_load: Receiving data for account %d, char id does not matches (%d != %d)!\n", aid, sd->status.char_id, cid);
		return -1;
	}

	count = RFIFOW(fd,12); //Skill count

	for (i = 0; i < count; i++) {
		struct skill_cooldown_data *data = (struct skill_cooldown_data *)RFIFOP(fd,14 + i * sizeof(struct skill_cooldown_data));

		if (skill_blockpc_start(sd, data->skill_id, data->tick))
			clif_skill_cooldown_list(sd, data);
	}

	return 0;
}

/*==========================================
 * Send rates and motd to char server [Wizputer]
 * S 2b16 <base rate>.L <job rate>.L <drop rate>.L
 *------------------------------------------*/
int chrif_ragsrvinfo(int base_rate, int job_rate, int drop_rate) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,14);
	WFIFOW(char_fd,0) = 0x2b16;
	WFIFOL(char_fd,2) = base_rate;
	WFIFOL(char_fd,6) = job_rate;
	WFIFOL(char_fd,10) = drop_rate;
	WFIFOSET(char_fd,14);

	return 0;
}


/*=========================================
 * Tell char-server charcter disconnected [Wizputer]
 *-----------------------------------------*/
int chrif_char_offline(struct map_session_data *sd) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,10);
	WFIFOW(char_fd,0) = 0x2b17;
	WFIFOL(char_fd,2) = sd->status.char_id;
	WFIFOL(char_fd,6) = sd->status.account_id;
	WFIFOSET(char_fd,10);

	return 0;
}
int chrif_char_offline_nsd(int account_id, int char_id) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,10);
	WFIFOW(char_fd,0) = 0x2b17;
	WFIFOL(char_fd,2) = char_id;
	WFIFOL(char_fd,6) = account_id;
	WFIFOSET(char_fd,10);

	return 0;
}

/*=========================================
 * Tell char-server to reset all chars offline [Wizputer]
 *-----------------------------------------*/
int chrif_flush_fifo(void) {
	chrif_check(-1);

	set_nonblocking(char_fd, 0);
	flush_fifos();
	set_nonblocking(char_fd, 1);

	return 0;
}

/*=========================================
 * Tell char-server to reset all chars offline [Wizputer]
 *-----------------------------------------*/
int chrif_char_reset_offline(void) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,2);
	WFIFOW(char_fd,0) = 0x2b18;
	WFIFOSET(char_fd,2);

	return 0;
}

/*=========================================
 * Tell char-server charcter is online [Wizputer]
 *-----------------------------------------*/
int chrif_char_online(struct map_session_data *sd) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,10);
	WFIFOW(char_fd,0) = 0x2b19;
	WFIFOL(char_fd,2) = sd->status.char_id;
	WFIFOL(char_fd,6) = sd->status.account_id;
	WFIFOSET(char_fd,10);

	return 0;
}


///Called when the connection to Char Server is disconnected.
void chrif_on_disconnect(void) {
	if (chrif_connected != 1)
		ShowWarning("Connection to Char Server lost.\n\n");
	chrif_connected = 0;

	other_mapserver_count = 0; //Reset counter, we receive ALL maps from all map-servers on reconnect
	map_eraseallipport();

	//Attempt to reconnect in a second [Skotlex]
	add_timer(gettick() + 1000, check_connect_char_server, 0, 0);
}


void chrif_update_ip(int fd) {
	uint32 new_ip;

	WFIFOHEAD(fd,6);

	new_ip = host2ip(char_ip_str);

	if (new_ip && new_ip != char_ip)
		char_ip = new_ip; //Update char_ip

	new_ip = clif_refresh_ip();

	if (!new_ip)
		return; //No change

	WFIFOW(fd,0) = 0x2736;
	WFIFOL(fd,2) = htonl(new_ip);
	WFIFOSET(fd,6);
}

//Pings the charserver
void chrif_keepalive(int fd) {
	WFIFOHEAD(fd,2);
	WFIFOW(fd,0) = 0x2b23;
	WFIFOSET(fd,2);
}
void chrif_keepalive_ack(int fd) {
	session[fd]->flag.ping = 0; /* Reset ping state, we received a packet */
}

/**
 * Received vip-data from char-serv, fill map-serv data
 * @param fd : char-serv file descriptor (link to char-serv)
 */
void chrif_parse_ack_vipActive(int fd) {
#ifdef VIP_ENABLE
	uint32 aid = RFIFOL(fd,2);
	uint32 vip_time = RFIFOL(fd,6);
	uint8 flag = RFIFOB(fd,10);
	uint32 group_id = RFIFOL(fd,11);
	TBL_PC *sd = map_id2sd(aid);
	bool changed = false;

	if (!sd)
		return;

	sd->group_id = group_id;
	pc_group_pc_load(sd);

	if (flag&0x2) //isgm
		clif_displaymessage(sd->fd,msg_txt(437)); // GM's cannot become a VIP.
	else {
		changed = (sd->vip.enabled != (flag&0x1));
		if (flag&0x1) { //isvip
			sd->vip.enabled = 1;
			sd->vip.time = vip_time;
			sd->storage.max_amount = battle_config.vip_storage_increase + MIN_STORAGE; //Increase storage size for VIP
			if (sd->storage.max_amount > MAX_STORAGE) {
				ShowError("intif_parse_ack_vipActive: Storage size for player %s (%d:%d) is larger than MAX_STORAGE. Storage size has been set to MAX_STORAGE.\n", sd->status.name, sd->status.account_id, sd->status.char_id);
				sd->storage.max_amount = MAX_STORAGE;
			}
		} else if (sd->vip.enabled) {
			sd->vip.enabled = 0;
			sd->vip.time = 0;
			sd->storage.max_amount = MIN_STORAGE;
			sd->special_state.no_gemstone = 0;
			clif_displaymessage(sd->fd, msg_txt(438)); // You are no longer VIP.
		}
	}

	if (((flag&0x4) || changed) && !sd->vip.disableshowrate) { //Show info if status changed
		clif_display_pinfo(sd, ZC_PERSONAL_INFOMATION);
		//clif_vip_display_info(sd, ZC_PERSONAL_INFOMATION_CHN);
	}
#endif
}

/**
 * ZA 0x2b2d
 * <cmd>.W <char_id>.L
 * Requets bonus_script datas
 * @param char_id
 * @author [Cydh]
 */
int chrif_bsdata_request(uint32 char_id) {
	chrif_check(-1);
	WFIFOHEAD(char_fd,6);
	WFIFOW(char_fd,0) = 0x2b2d;
	WFIFOL(char_fd,2) = char_id;
	WFIFOSET(char_fd,6);
	return 0;
}

/**
 * ZA 0x2b2e
 * <cmd>.W <len>.W <char_id>.L <count>.B { <bonus_script>.?B }
 * Stores bonus_script data(s) to the table
 * @param sd
 * @author [Cydh]
 */
int chrif_bsdata_save(struct map_session_data *sd, bool quit) {
	uint8 i = 0;

	chrif_check(-1);

	if (!sd)
		return 0;

	//Removing
	if (quit && sd->bonus_script.head) {
		uint16 flag = BSF_REM_ON_LOGOUT; //Remove bonus when logout

		if (battle_config.debuff_on_logout&1) //Remove negative buffs
			flag |= BSF_REM_DEBUFF;
		if (battle_config.debuff_on_logout&2) //Remove positive buffs
			flag |= BSF_REM_BUFF;
		pc_bonus_script_clear(sd, flag);
	}

	//ShowInfo("Saving %d bonus script for CID=%d\n", sd->bonus_script.count, sd->status.char_id);

	WFIFOHEAD(char_fd,9 + sd->bonus_script.count * sizeof(struct bonus_script_data));
	WFIFOW(char_fd,0) = 0x2b2e;
	WFIFOL(char_fd,4) = sd->status.char_id;

	if (sd->bonus_script.count) {
		unsigned int tick = gettick();
		struct linkdb_node *node = NULL;

		for (node = sd->bonus_script.head; node && i < MAX_PC_BONUS_SCRIPT; node = node->next) {
			const struct TimerData *timer = NULL;
			struct bonus_script_data bs;
			struct s_bonus_script_entry *entry = (struct s_bonus_script_entry *)node->data;

			if (!entry || !(timer = get_timer(entry->tid)) || DIFF_TICK(timer->tick,tick) < 0)
				continue;

			memset(&bs, 0, sizeof(bs));
			safestrncpy(bs.script_str, StringBuf_Value(entry->script_buf), StringBuf_Length(entry->script_buf) + 1);
			bs.tick = DIFF_TICK(timer->tick, tick);
			bs.flag = entry->flag;
			bs.type = entry->type;
			bs.icon = entry->icon;
			memcpy(WFIFOP(char_fd, 9 + i * sizeof(struct bonus_script_data)), &bs, sizeof(struct bonus_script_data));
			i++;
		}

		if (i != sd->bonus_script.count && sd->bonus_script.count > MAX_PC_BONUS_SCRIPT)
			ShowWarning("Only allowed to save %d (mmo.h::MAX_PC_BONUS_SCRIPT) bonus script each player.\n", MAX_PC_BONUS_SCRIPT);
	}

	WFIFOB(char_fd,8) = i;
	WFIFOW(char_fd,2) = 9 + sd->bonus_script.count * sizeof(struct bonus_script_data);
	WFIFOSET(char_fd, WFIFOW(char_fd,2));
	return 0;
}

/**
 * AZ 0x2b2f
 * <cmd>.W <len>.W <cid>.L <count>.B { <bonus_script_data>.?B }
 * Bonus script received, set to player
 * @param fd
 * @author [Cydh]
 */
int chrif_bsdata_received(int fd) {
	struct map_session_data *sd;
	uint32 cid = RFIFOL(fd,4);
	uint8 count = 0;

	sd = map_charid2sd(cid);

	if (!sd) {
		ShowError("chrif_bsdata_received: Player with CID %d not found!\n",cid);
		return -1;
	}

	if ((count = RFIFOB(fd,8))) {
		uint8 i = 0;

		//ShowInfo("Loaded %d bonus script for CID=%d\n", count, sd->status.char_id);

		for (i = 0; i < count; i++) {
			struct bonus_script_data *bs = (struct bonus_script_data *)RFIFOP(fd,9 + i * sizeof(struct bonus_script_data));
			struct s_bonus_script_entry *entry = NULL;

			if (bs->script_str[0] == '\0' || !bs->tick)
				continue;

			if (!(entry = pc_bonus_script_add(sd, bs->script_str, bs->tick, (enum si_type)bs->icon, bs->flag, bs->type)))
				continue;

			linkdb_insert(&sd->bonus_script.head, (void *)((intptr_t)entry), entry);
		}

		if (sd->bonus_script.head)
			status_calc_pc(sd,SCO_NONE);
	}
	return 0;
}

/*==========================================
 *
 *------------------------------------------*/
int chrif_parse(int fd) {
	int packet_len;

	//Only process data from the char-server
	if ( fd != char_fd ) {
		ShowDebug("chrif_parse: Disconnecting invalid session #%d (is not the char-server)\n", fd);
		do_close(fd);
		return 0;
	}

	if ( session[fd]->flag.eof ) {
		do_close(fd);
		char_fd = -1;
		chrif_on_disconnect();
		return 0;
	} else if ( session[fd]->flag.ping ) { /* We've reached stall time */
		if( DIFF_TICK(last_tick, session[fd]->rdata_tick) > (stall_time * 2) ) { /* We can't wait any longer */
			set_eof(fd);
			return 0;
		} else if( session[fd]->flag.ping != 2 ) { /* We haven't sent ping out yet */
			chrif_keepalive(fd);
			session[fd]->flag.ping = 2;
		}
	}

	while ( RFIFOREST(fd) >= 2 ) {
		int cmd = RFIFOW(fd,0);

		if ( cmd < 0x2af8 || cmd >= 0x2af8 + ARRAYLENGTH(packet_len_table) || packet_len_table[cmd-0x2af8] == 0 ) {
			int r = intif_parse(fd); // Passed on to the intif

			if ( r == 1 ) continue;	// Treated in intif
			if ( r == 2 ) return 0;	// Didn't have enough data (len==-1)

			ShowWarning("chrif_parse: session #%d, intif_parse failed (unrecognized command 0x%.4x).\n", fd, cmd);
			set_eof(fd);
			return 0;
		}

		if ( (packet_len = packet_len_table[cmd-0x2af8]) == -1 ) { // Dynamic-length packet, second WORD holds the length
			if (RFIFOREST(fd) < 4)
				return 0;
			packet_len = RFIFOW(fd,2);
		}

		if ( (int)RFIFOREST(fd) < packet_len )
			return 0;

		//ShowDebug("Received packet 0x%4x (%d bytes) from char-server (connection %d)\n", RFIFOW(fd,0), packet_len, fd);

		switch ( cmd ) {
			case 0x2af9: chrif_connectack(fd); break;
			case 0x2afb: chrif_sendmapack(fd); break;
			case 0x2afd: chrif_authok(fd); break;
			case 0x2b00: map_setusers(RFIFOL(fd,2)); chrif_keepalive(fd); break;
			case 0x2b03: clif_charselectok(RFIFOL(fd,2), RFIFOB(fd,6)); break;
			case 0x2b04: chrif_recvmap(fd); break;
			case 0x2b06: chrif_changemapserverack(RFIFOL(fd,2), RFIFOL(fd,6), RFIFOL(fd,10), RFIFOL(fd,14), RFIFOW(fd,18), RFIFOW(fd,20), RFIFOW(fd,22), RFIFOL(fd,24), RFIFOW(fd,28)); break;
			case 0x2b09: map_addnickdb(RFIFOL(fd,2), (char *)RFIFOP(fd,6)); break;
			case 0x2b0b: chrif_skillcooldown_load(fd); break;
			case 0x2b0d: chrif_changedsex(fd); break;
			case 0x2b0f: chrif_ack_login_req(RFIFOL(fd,2), (char *)RFIFOP(fd,6), RFIFOW(fd,30), RFIFOW(fd,32)); break;
			case 0x2b12: chrif_divorceack(RFIFOL(fd,2), RFIFOL(fd,6)); break;
			case 0x2b13: socket_datasync(fd, false); break;
			case 0x2b14: chrif_ban(fd); break;
			case 0x2b1b: chrif_recvfamelist(fd); break;
			case 0x2b1d: chrif_load_scdata(fd); break;
			case 0x2b1e: chrif_update_ip(fd); break;
			case 0x2b1f: chrif_disconnectplayer(fd); break;
			case 0x2b20: chrif_removemap(fd); break;
			case 0x2b21: chrif_save_ack(fd); break;
			case 0x2b22: chrif_updatefamelist_ack(fd); break;
			case 0x2b24: chrif_keepalive_ack(fd); break;
			case 0x2b25: chrif_deadopt(RFIFOL(fd,2), RFIFOL(fd,6), RFIFOL(fd,10)); break;
			case 0x2b27: chrif_authfail(fd); break;
			case 0x2b2b: chrif_parse_ack_vipActive(fd); break;
			case 0x2b2f: chrif_bsdata_received(fd); break;
			default:
				ShowError("chrif_parse : unknown packet (session #%d): 0x%x. Disconnecting.\n", fd, cmd);
				set_eof(fd);
				return 0;
		}
		if ( fd == char_fd ) //There's the slight chance we lost the connection during parse, in which case this would segfault if not checked [Skotlex]
			RFIFOSKIP(fd, packet_len);
	}

	return 0;
}

// Unused
int send_usercount_tochar(int tid, unsigned int tick, int id, intptr_t data) {
	chrif_check(-1);

	WFIFOHEAD(char_fd,4);
	WFIFOW(char_fd,0) = 0x2afe;
	WFIFOW(char_fd,2) = map_usercount();
	WFIFOSET(char_fd,4);
	return 0;
}

/*==========================================
 * timerFunction
 * �Send to char the number of client connected to map
 *------------------------------------------*/
int send_users_tochar(void) {
	int users = 0, i = 0;
	struct map_session_data *sd;
	struct s_mapiterator *iter;

	chrif_check(-1);

	users = map_usercount();

	WFIFOHEAD(char_fd, 6+8*users);
	WFIFOW(char_fd,0) = 0x2aff;

	iter = mapit_getallusers();

	for( sd = (TBL_PC *)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC *)mapit_next(iter) ) {
		WFIFOL(char_fd,6+8*i) = sd->status.account_id;
		WFIFOL(char_fd,6+8*i+4) = sd->status.char_id;
		i++;
	}

	mapit_free(iter);

	WFIFOW(char_fd,2) = 6 + 8*users;
	WFIFOW(char_fd,4) = users;
	WFIFOSET(char_fd, 6+8*users);

	return 0;
}

/*==========================================
 * timerFunction
 * Chk the connection to char server, (if it down)
 *------------------------------------------*/
static int check_connect_char_server(int tid, unsigned int tick, int id, intptr_t data) {
	static int displayed = 0;

	if (char_fd <= 0 || session[char_fd] == NULL) {
		if (!displayed) {
			ShowStatus("Attempting to connect to Char Server. Please wait.\n");
			displayed = 1;
		}

		chrif_state = 0;
		char_fd = make_connection(char_ip, char_port, false, 10);
		
		if (char_fd == -1) //Attempt to connect later. [Skotlex]
			return 0;

		session[char_fd]->func_parse = chrif_parse;
		session[char_fd]->flag.server = 1;
		realloc_fifo(char_fd, FIFOSIZE_SERVERLINK, FIFOSIZE_SERVERLINK);

		chrif_connect(char_fd);
		chrif_connected = (chrif_state == 2);
		srvinfo = 0;
	} else {
		if (srvinfo == 0) {
			chrif_ragsrvinfo(battle_config.base_exp_rate, battle_config.job_exp_rate, battle_config.item_rate_common);
			srvinfo = 1;
		}
	}
	if (chrif_isconnected())
		displayed = 0;
	return 0;
}

/*==========================================
 * Asks char server to remove friend_id from the friend list of char_id
 *------------------------------------------*/
int chrif_removefriend(int char_id, int friend_id) {
	chrif_check(-1);
	WFIFOHEAD(char_fd,10);
	WFIFOW(char_fd,0) = 0x2b07;
	WFIFOL(char_fd,2) = char_id;
	WFIFOL(char_fd,6) = friend_id;
	WFIFOSET(char_fd,10);
	return 0;
}

/**
 * @see DBApply
 */
int auth_db_final(DBKey key, DBData *data, va_list ap) {
	struct auth_node *node = (struct auth_node *)db_data2ptr(data);

	if (node->char_dat)
		aFree(node->char_dat);

	if (node->sd)
		aFree(node->sd);

	ers_free(auth_db_ers, node);

	return 0;
}

/*==========================================
 * �Destructor
 *------------------------------------------*/
void do_final_chrif(void) {

	if( char_fd != -1 ) {
		do_close(char_fd);
		char_fd = -1;
	}

	auth_db->destroy(auth_db, auth_db_final);

	ers_destroy(auth_db_ers);
}

/*==========================================
 *
 *------------------------------------------*/
void do_init_chrif(void) {
	if( sizeof(struct mmo_charstatus) > 0xFFFF ) {
		ShowError("mmo_charstatus size = %d is too big to be transmitted. (must be below 0xFFFF)\n",
			sizeof(struct mmo_charstatus));
		exit(EXIT_FAILURE);
	}

	if( sizeof(struct s_storage) > 0xFFFF ) {
		ShowError("s_storage size = %d is too big to be transmitted. (must be below 0xFFFF)\n", sizeof(struct s_storage));
		exit(EXIT_FAILURE);
	}

	if( (sizeof(struct bonus_script_data) * MAX_PC_BONUS_SCRIPT) > 0xFFFF ) {
		ShowError("bonus_script_data size = %d is too big, please reduce MAX_PC_BONUS_SCRIPT (%d) size. (must be below 0xFFFF).\n",
			(sizeof(struct bonus_script_data) * MAX_PC_BONUS_SCRIPT), MAX_PC_BONUS_SCRIPT);
		exit(EXIT_FAILURE);
	}

	auth_db = idb_alloc(DB_OPT_BASE);
	auth_db_ers = ers_new(sizeof(struct auth_node),"chrif.c::auth_db_ers",ERS_OPT_NONE);

	add_timer_func_list(check_connect_char_server, "check_connect_char_server");
	add_timer_func_list(auth_db_cleanup, "auth_db_cleanup");

	// establish map-char connection if not present
	add_timer_interval(gettick() + 1000, check_connect_char_server, 0, 0, 10 * 1000);

	// wipe stale data for timed-out client connection requests
	add_timer_interval(gettick() + 1000, auth_db_cleanup, 0, 0, 30 * 1000);

	// send the user count every 10 seconds, to hide the charserver's online counting problem
	add_timer_interval(gettick() + 1000, send_usercount_tochar, 0, 0, UPDATE_INTERVAL);
}
