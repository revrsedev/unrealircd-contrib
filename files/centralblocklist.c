/* Central blocklist
 * (C) Copyright 2023 Bram Matthys and The UnrealIRCd Team
 * License: GPLv2
 */

/*** <<<MODULE MANAGER START>>>
module
{
	documentation "https://www.unrealircd.org/docs/Central_Blocklist";

	// This is displayed in './unrealircd module info ..' and also if compilation of the module fails:
	troubleshooting "Please report at https://bugs.unrealircd.org/ if this module fails to compile";

	// Minimum version necessary for this module to work:
	min-unrealircd-version "6.1.2";

	// Maximum version
	max-unrealircd-version "6.*";

	post-install-text {
		"The module is installed. See https://www.unrealircd.org/docs/Central_Blocklist";
		"for the configuration that you need to add. One important aspect is getting";
		"an API Key, which is a process that (as of October 2023) is not open to everyone.";
	}
}
*** <<<MODULE MANAGER END>>>
*/

#include "unrealircd.h"

ModuleHeader MOD_HEADER
  = {
	"third/centralblocklist",
	"0.9.6",
	"Check users at central blocklist",
	"UnrealIRCd Team",
	"unrealircd-6",
    };

ModDataInfo *centralblocklist_md = NULL;
Module *cbl_module = NULL;

#define CBL_URL	 "https://centralblocklist.unrealircd.org/api/v0"

/* For tracking current HTTPS requests */
typedef struct CBLTransfer CBLTransfer;
struct CBLTransfer
{
	CBLTransfer *prev, *next;
	char id[IDLEN+1];
};

struct cfgstruct {
	char *api_key;
	int score;
	BanAction *ban_action;
	char *ban_reason;
	long ban_time;
	int max_downloads;
	SecurityGroup *except;
};

static struct cfgstruct cfg;

struct reqstruct {
	char api_key;
};
static struct reqstruct req;

CBLTransfer *cbltransfers = NULL;

/* Forward declarations */
int cbl_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int cbl_config_posttest(int *errs);
int cbl_config_run(ConfigFile *cf, ConfigEntry *ce, int type);
int cbl_packet(Client *from, Client *to, Client *intended_to, char **msg, int *len);
//int cbl_prelocalconnect(Client *client);
int cbl_is_handshake_finished(Client *client);
void cbl_download_complete(const char *url, const char *file, const char *memory, int memory_len, const char *errorbuf, int cached, void *rs_key);
void cbl_mdata_free(ModData *m);
int cbl_start_request(Client *client);

#define CBLRAW(x)	(moddata_local_client(x, centralblocklist_md).ptr)
#define CBL(x)		((json_t *)(moddata_local_client(x, centralblocklist_md).ptr))

#define alloc_cbl_if_needed(x)	do { \
					if (!moddata_local_client(x, centralblocklist_md).ptr) \
						moddata_local_client(x, centralblocklist_md).ptr = json_object(); \
				   } while(0)

CMD_OVERRIDE_FUNC(cbl_override);

/* Default config */
static void init_config(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.score = 5;
	safe_strdup(cfg.ban_reason, "Rejected by central blocklist");
	cfg.max_downloads = 100;
	cfg.ban_action = banact_value_to_struct(BAN_ACT_SHUN);
	cfg.ban_time = 900;
	// and the default except block
	cfg.except = safe_alloc(sizeof(SecurityGroup));
	cfg.except->reputation_score = 2016; /* 7 days unregged, or 3.5 days identified */
	cfg.except->identified = 1;
	// exception masks
	unreal_add_mask_string(&cfg.except->mask, "*.irccloud.com");
	// exception IPs
#ifndef DEBUGMODE
	add_name_list(cfg.except->ip, "127.0.0.1");
	add_name_list(cfg.except->ip, "192.168.*");
	add_name_list(cfg.except->ip, "10.*");
#endif
}

static void free_config(void)
{
	safe_free(cfg.ban_reason);
	free_security_group(cfg.except);
	safe_free_all_ban_actions(cfg.ban_action);
	memset(&cfg, 0, sizeof(cfg)); /* needed! */
}


MOD_TEST()
{
	memset(&req, 0, sizeof(req));
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, cbl_config_test);
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGPOSTTEST, 0, cbl_config_posttest);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	ModDataInfo mreq;

	cbl_module = modinfo->handle;

	init_config();

	memset(&mreq, 0, sizeof(mreq));
	mreq.name = "central-blocklist";
	mreq.type = MODDATATYPE_LOCAL_CLIENT;
	mreq.free = cbl_mdata_free;
	centralblocklist_md = ModDataAdd(modinfo->handle, mreq);
	if (!centralblocklist_md)
	{
		config_error("[central-blocklist] failed adding moddata");
		return MOD_FAILED;
	}
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, cbl_config_run);
	//HookAdd(modinfo->handle, HOOKTYPE_PRE_LOCAL_CONNECT, 0, cbl_prelocalconnect);
	HookAdd(modinfo->handle, HOOKTYPE_IS_HANDSHAKE_FINISHED, INT_MAX, cbl_is_handshake_finished);
	return MOD_SUCCESS;
}

void do_command_overrides(ModuleInfo *modinfo)
{
	RealCommand *cmd;
	int i;

	for (i = 0; i < 256; i++)
	{
		for (cmd = CommandHash[i]; cmd; cmd = cmd->next)
		{
			if (cmd->flags & CMD_UNREGISTERED)
				CommandOverrideAdd(modinfo->handle, cmd->cmd, -1, cbl_override);
		}
	}
}

CBLTransfer *add_cbl_transfer(Client *client)
{
	CBLTransfer *c = safe_alloc(sizeof(CBLTransfer));
	strlcpy(c->id, client->id, sizeof(c->id));
	AddListItem(c, cbltransfers);
	return c;
}

void del_cbl_transfer(CBLTransfer *c)
{
	DelListItem(c, cbltransfers);
	safe_free(c);
}

void cbl_cancel_all_transfers(void)
{
	CBLTransfer *c, *c_next;
	Client *client;

	for (c = cbltransfers; c; c = c_next)
	{
		json_t *cbl;
		c_next = c->next;

		/* Mark for retry in next module (re)load */
		client = find_client(c->id, NULL);
		if (client && (cbl = CBL(client)))
		{
			if (json_object_get(cbl, "request_sent"))
			{
				json_object_del(cbl, "request_sent"); /* no longer marked as sent */
				json_object_set_new(cbl, "request_need_retry", json_integer(1)); /* marked for retry */
			}
		}

		/* Cancel the request (this is what matters most) */
		url_cancel_handle_by_callback_data(c);

		safe_free(c);
	}
	cbltransfers = NULL;
}

void cbl_retry_canceled_requests(void)
{
	Client *client, *next = NULL;

	list_for_each_entry_safe(client, next, &unknown_list, lclient_node)
	{
		json_t *cbl = CBL(client);
		if (cbl && json_object_get(cbl, "request_need_retry"))
		{
			json_object_del(cbl, "request_need_retry");
			cbl_start_request(client);
		}
	}
}

MOD_LOAD()
{
	do_command_overrides(modinfo);
	cbl_retry_canceled_requests();
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	cbl_cancel_all_transfers();
	free_config();
	return MOD_SUCCESS;
}

/** Test the set::central-blocklist configuration */
int cbl_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep, *cepp;

	if (type != CONFIG_SET)
		return 0;
	
	/* We are only interrested in set::central-blocklist.. */
	if (!ce || !ce->name || strcmp(ce->name, "central-blocklist"))
		return 0;
	
	for (cep = ce->items; cep; cep = cep->next)
	{
		if (!strcmp(cep->name, "api-key"))
		{
			req.api_key = 1;
		} else
		if (!strcmp(cep->name, "except"))
		{
			test_match_block(cf, cep, &errors);
		} else
		if (!cep->value)
		{
			config_error("%s:%i: set::central-blocklist::%s with no value",
				cep->file->filename, cep->line_number, cep->name);
			errors++;
		} else
		if (!strcmp(cep->name, "score"))
		{
			int v = atoi(cep->value);
			if ((v < 1) || (v > 99))
			{
				config_error("%s:%i: set::central-blocklist::score: must be between 1 - 99 (got: %d)",
					cep->file->filename, cep->line_number, v);
				errors++;
			}
		} else
		if (!strcmp(cep->name, "max-downloads"))
		{
			int v = atoi(cep->value);
			if ((v < 1) || (v > 500))
			{
				config_error("%s:%i: set::central-blocklist::score: must be between 1 - 500 (got: %d)",
					cep->file->filename, cep->line_number, v);
				errors++;
			}
		} else
		if (!strcmp(cep->name, "ban-action"))
		{
			errors += test_ban_action_config(cep);
		} else
		if (!strcmp(cep->name, "ban-reason"))
		{
		} else
		if (!strcmp(cep->name, "ban-time"))
		{
		} else
		{
			config_error("%s:%i: unknown directive set::central-blocklist::%s",
				cep->file->filename, cep->line_number, cep->name);
			errors++;
			continue;
		}
	}
	
	*errs = errors;
	return errors ? -1 : 1;
}

int cbl_config_posttest(int *errs)
{
	int errors = 0;

	if (!req.api_key)
	{
		config_error("set::central-blocklist::api-key missing");
		errors++;
	}

	*errs = errors;
	return errors ? -1 : 1;
}

/* Configure ourselves based on the set::central-blocklist settings */
int cbl_config_run(ConfigFile *cf, ConfigEntry *ce, int type)
{
	ConfigEntry *cep, *cepp;

	if (type != CONFIG_SET)
		return 0;
	
	/* We are only interrested in set::central-blocklist.. */
	if (!ce || !ce->name || strcmp(ce->name, "central-blocklist"))
		return 0;
	
	for (cep = ce->items; cep; cep = cep->next)
	{
		if (!strcmp(cep->name, "api-key"))
		{
			safe_strdup(cfg.api_key, cep->value);
		} else
		if (!strcmp(cep->name, "score"))
		{
			cfg.score = atoi(cep->value);
		} else
		if (!strcmp(cep->name, "max-downloads"))
		{
			cfg.max_downloads = atoi(cep->value);
		} else
		if (!strcmp(cep->name, "ban-action"))
		{
			parse_ban_action_config(cep, &cfg.ban_action);
		} else
		if (!strcmp(cep->name, "ban-reason"))
		{
			safe_strdup(cfg.ban_reason, cep->value);
		} else
		if (!strcmp(cep->name, "ban-time"))
		{
			cfg.ban_time = config_checkval(cep->value, CFG_TIME);
		} else
		if (!strcmp(cep->name, "except"))
		{
			if (cfg.except)
			{
				free_security_group(cfg.except);
				cfg.except = NULL;
			}
			conf_match_block(cf, cep, &cfg.except);
		}
	}
	return 1;
}

void show_client_json(Client *client)
{
	char *json_serialized;
	json_serialized = json_dumps(CBL(client), JSON_COMPACT);

	unreal_log(ULOG_DEBUG, "central-blocklist", "DEBUG_CENTRAL_BLOCKLIST", client,
		   "OUT: $data",
		   log_data_string("data", json_serialized));
	safe_free(json_serialized);
}

void cbl_add_client_info(Client *client)
{
	char buf[BUFSIZE+1];
	json_t *cbl = CBL(client);
	json_t *child = json_object();
	const char *str;

	json_object_set_new(cbl, "server", json_string_unreal(me.name));
	json_object_set_new(cbl, "module_version", json_string_unreal(cbl_module->header->version));

	json_object_set_new(cbl, "client", child);

	//// THE FOLLOWING IS TAKEN FROM src/json.c AND MODIFIED /////
	
	/* First the information that is available for ALL client types: */
	json_object_set_new(child, "name", json_string_unreal(client->name));
	json_object_set_new(child, "id", json_string_unreal(client->id));

	/* hostname is available for all, it just depends a bit on whether it is DNS or IP */
	if (client->user && *client->user->realhost)
		json_object_set_new(child, "hostname", json_string_unreal(client->user->realhost));
	else if (client->local && *client->local->sockhost)
		json_object_set_new(child, "hostname", json_string_unreal(client->local->sockhost));
	else
		json_object_set_new(child, "hostname", json_string_unreal(GetIP(client)));

	/* same for ip, is there for all (well, some services pseudo-users may not have one) */
	json_object_set_new(child, "ip", json_string_unreal(client->ip));

	/* client.details is always available: it is nick!user@host, nick@host, server@host
	 * server@ip, or just server.
	 */
	if (client->user)
	{
		snprintf(buf, sizeof(buf), "%s!%s@%s", client->name, client->user->username, client->user->realhost);
		json_object_set_new(child, "details", json_string_unreal(buf));
	} else if (client->ip) {
		if (*client->name)
			snprintf(buf, sizeof(buf), "%s@%s", client->name, client->ip);
		else
			snprintf(buf, sizeof(buf), "[%s]", client->ip);
		json_object_set_new(child, "details", json_string_unreal(buf));
	} else {
		json_object_set_new(child, "details", json_string_unreal(client->name));
	}

	if (client->local && client->local->listener)
		json_object_set_new(child, "server_port", json_integer(client->local->listener->port));
	if (client->local && client->local->port)
		json_object_set_new(child, "client_port", json_integer(client->local->port));

	if (client->user)
	{
		char buf[512];
		const char *str;
		/* client.user */
		json_t *user = json_object();
		json_object_set_new(child, "user", user);

		json_object_set_new(user, "username", json_string_unreal(client->user->username));
		if (!BadPtr(client->info))
			json_object_set_new(user, "realname", json_string_unreal(client->info));
		json_object_set_new(user, "reputation", json_integer(GetReputation(client)));
	}

	if ((str = moddata_client_get(client, "tls_cipher")))
	{
		json_t *tls = json_object();
		json_object_set_new(child, "tls", tls);
		json_object_set_new(tls, "cipher", json_string_unreal(str));
		if (client->local->sni_servername)
			json_object_set_new(tls, "sni_servername", json_string_unreal(client->local->sni_servername));
	}
}

CMD_OVERRIDE_FUNC(cbl_override)
{
	json_t *cbl;
	json_t *cmds;
	json_t *item;
	char timebuf[64];
	char number[32];

	if (!MyConnect(client) ||
	    !IsUnknown(client) ||
	    !strcmp(ovr->command->cmd, "PASS") ||
	    !strcmp(ovr->command->cmd, "AUTHENTICATE"))
	{
		CALL_NEXT_COMMAND_OVERRIDE();
		return;
	}

	alloc_cbl_if_needed(client);
	cbl = CBL(client);

	cmds = json_object_get(cbl, "commands");
	if (!cmds)
	{
		cmds = json_object();
		json_object_set_new(cbl, "commands", cmds);
	}

	strlcpy(timebuf, timestamp_iso8601_now(), sizeof(timebuf));
	snprintf(number, sizeof(number), "%lld", client->local->traffic.messages_received);

	item = json_object();
	json_object_set_new(item, "time", json_string_unreal(timebuf));
	json_object_set_new(item, "command", json_string_unreal(ovr->command->cmd));
	json_object_set_new(item, "raw", json_string_unreal(backupbuf));
	json_object_set_new(cmds, number, item);

	CALL_NEXT_COMMAND_OVERRIDE();
}

int cbl_start_request(Client *client)
{
	int num;
	char *json_serialized;
	NameValuePrioList *headers = NULL;
	CBLTransfer *c;
	json_t *cbl = CBL(client);

	if (json_object_get(cbl, "request_sent"))
		return 0; /* Handshake is NOT finished yet, HTTP request already in progress */

	num = downloads_in_progress();
	if (num > cfg.max_downloads)
	{
		unreal_log(ULOG_WARNING, "central-blocklist", "CENTRAL_BLOCKLIST_TOO_MANY_CONCURRENT_REQUESTS", client,
			   "Already $num_requests HTTP(S) requests in progress, not checking user $client.details",
			   log_data_integer("num_requests", num));
		return 1; // Let user in -sigh-
	}

	if (!json_object_get(cbl, "client"))
		cbl_add_client_info(client);
#ifdef DEBUGMODE
	show_client_json(client);
#endif
	json_object_set_new(cbl, "request_sent", json_string_unreal(timestamp_iso8601_now()));

	json_serialized = json_dumps(cbl, JSON_COMPACT);
	if (!json_serialized)
	{
		unreal_log(ULOG_WARNING, "central-blocklist", "CENTRAL_BLOCKLIST_BUG_SERIALIZE", client,
			   "Unable to serialize JSON request. Weird.");
		return 1; // wtf?
	}

	add_nvplist(&headers, 0, "Content-Type", "application/json; charset=utf-8");
	add_nvplist(&headers, 0, "X-API-Key", cfg.api_key);
	c = add_cbl_transfer(client);
	url_start_async(CBL_URL, HTTP_METHOD_POST, json_serialized, headers, 0, 0, cbl_download_complete, c, CBL_URL, 1);
	safe_free(json_serialized);
	safe_free_nvplist(headers);

	return 0; /* Handshake is NOT finished yet, HTTPS request initiated */
}

int cbl_is_handshake_finished(Client *client)
{
	if (!CBL(client))
		return 1; // something went wrong or we are finished with this, let the user through

	/* Missing something, pretend we are finished and don't handle */
	if (!(client->user && *client->user->username && client->name[0] && IsNotSpoof(client)))
		return 1;

	/* User is exempt */
	if (user_allowed_by_security_group(client, cfg.except))
		return 1;

	return cbl_start_request(client);
}

// TODO: event for (quicker) timeouts ;)

void cbl_allow(Client *client)
{
	if (CBL(client))
	{
		json_decref(CBL(client));
		CBLRAW(client) = NULL;
	}

	if (is_handshake_finished(client))
		register_user(client);
}

void cbl_download_complete(const char *url, const char *file, const char *memory, int memory_len, const char *errorbuf, int cached, void *rs_key)
{
	ConfigFile *cfptr;
	int errors;
	int num_rules, active_rules;
	CBLTransfer *transfer;
	char buf[8192];
	json_t *response = NULL; // complete JSON response
	int spam_score = 0; // spam score, can be negative too
	json_error_t jerr;
	Client *client;

	transfer = (CBLTransfer *)rs_key;
	client = find_client(transfer->id, NULL); // can be NULL
	del_cbl_transfer(transfer);

	if (!client)
		return; /* Nothing to do anymore, client already left */

	if (errorbuf || !memory)
	{
		unreal_log(ULOG_DEBUG, "central-blocklist", "DEBUG_CENTRAL_BLOCKLIST", client,
		           "CBL ERROR: $error",
		           log_data_string("error", errorbuf ? errorbuf : "No data returned"));
		goto cbl_failure;
	}

	strlncpy(buf, memory, sizeof(buf), memory_len);

#ifdef DEBUGMODE
	unreal_log(ULOG_DEBUG, "central-blocklist", "DEBUG_CENTRAL_BLOCKLIST", client,
	           "CBL Got response for $client: $buf",
	           log_data_string("buf", buf));
#endif

	// NOTE: if we didn't have that debug from above, we could avoid the strlncpy and use json_loadb here
	response = json_loads(buf, JSON_REJECT_DUPLICATES, &jerr);
	if (!response)
	{
		unreal_log(ULOG_DEBUG, "central-blocklist", "DEBUG_CENTRAL_BLOCKLIST", client,
		           "CBL ERROR: JSON parse error");
		goto cbl_failure;
	}

	spam_score = json_object_get_integer(response, "spam_score", 0);
	safe_json_decref(response);

	if (spam_score < cfg.score)
	{
		unreal_log(ULOG_DEBUG, "central-blocklist", "DEBUG_CENTRAL_BLOCKLIST", client,
                           "CBL: Client $client.details is allowed (score $spam_score)",
                           log_data_integer("spam_score", spam_score));
		cbl_allow(client);
	} else 
	{
		unreal_log(ULOG_INFO, "central-blocklist", "CBL_REJECTED_USER", client,
                           "CBL: Client $client.details is rejected by central-blocklist (score $spam_score)",
                           log_data_integer("spam_score", spam_score));
		if (take_action(client, cfg.ban_action, cfg.ban_reason, cfg.ban_time, 0, NULL) <= BAN_ACT_WARN)
			goto cbl_failure; /* only warn/report/set/stop = allow client through */
	}
	return;

cbl_failure:
	if (response)
		json_decref(response);
	cbl_allow(client);
}

void cbl_mdata_free(ModData *m)
{
	json_t *j = (json_t *)m->ptr;

	if (j)
	{
		json_decref(j);
		m->ptr = NULL;
	}
}
