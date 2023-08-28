/*
  Licence: GPLv3
  Copyright 2023 Ⓒ Valerie Pond
  
  Taken from DalekIRC
*/
/*** <<<MODULE MANAGER START>>>
module
{
		documentation "https://github.com/ValwareIRC/valware-unrealircd-mods/blob/main/logged-in-from/README.md";
		troubleshooting "In case of problems, documentation or e-mail me at v.a.pond@outlook.com";
		min-unrealircd-version "6.*";
		max-unrealircd-version "6.*";
		post-install-text {
				"The module is installed. Now all you need to do is add a loadmodule line:";
				"loadmodule \"third/logged-in-from\";";
				"And /REHASH the IRCd.";
				"The module does not need any other configuration.";
		}
}
*** <<<MODULE MANAGER END>>>
*/

#include "unrealircd.h"

/** "Logged in from" whois fields */
int loggedinfrom_whois(Client *requester, Client *acptr, NameValuePrioList **list);

/* Our module header */
ModuleHeader MOD_HEADER = {
	"third/logged-in-from",
	"1.1",
	"Extra /WHOIS information pertaining to account usage.",
	"Valware",
	"unrealircd-6",
};

/** Module initialization */
MOD_INIT() {
	MARK_AS_GLOBAL_MODULE(modinfo);

	/* Add our hooks */
	HookAdd(modinfo->handle, HOOKTYPE_WHOIS, 0, loggedinfrom_whois);

	return MOD_SUCCESS; // hooray
}
/** Called upon module load */
MOD_LOAD()
{
	
	return MOD_SUCCESS;
}

/** Called upon unload */
MOD_UNLOAD()
{
	return MOD_SUCCESS;
}

/**
 * In this whois hook, we let the user see their own "places they are logged in from".
 * Opers can also see this information.
*/
int loggedinfrom_whois(Client *requester, Client *acptr, NameValuePrioList **list)
{
	Client *client;
	char buf[512];

	if (!IsLoggedIn(acptr)) // not logged in, return
		return 0;

	if (!IsOper(requester) && strcasecmp(acptr->user->account,requester->user->account)) // only show to the self
		return 0;
	
	int i = 1;

	/**
	 * We go through each user on the network looking for people who are logged into the
	 * same account.
	*/
	list_for_each_entry(client, &client_list, client_node)
	{
		if (!strcasecmp(client->user->account,acptr->user->account))
		{
			add_nvplist_numeric_fmt(list, 999900 + i, "loggedin", acptr, 320, "%s :is logged in from %s!%s@%s (%i)%s",
									acptr->name,
									client->name,
									client->user->username,
									client->user->realhost,
									i,
									(client == requester) ? " (You)" : "");
			++i;
		}
	}

	/**
	 * We show the user how many places they're logged in, and then show the ^ above output (lower prio)
	*/
	if (acptr->user->account)
		add_nvplist_numeric_fmt(list, 999900, "loggedin", acptr, 320, "%s :is logged in from \x02%i place%s\x02:", acptr->name, i - 1, (i-1 == 1) ? "" : "s");
	

	/**
	 * This is where we add some extended information so that people logged in from multiple 
	 * places may see more information about where they're logged in.
	*/
	if (acptr != requester && !IsOper(requester)) // wouldn't otherwise get to see their extended information...
	{
		// we add their real host and ip as if it were there all along :')
		add_nvplist_numeric_fmt(list, -90000, "realhost", acptr, 378, "%s :is connecting from *@%s %s",
									acptr->name,
									acptr->user->realhost,
									acptr->ip);

		// and show what modes they're using.
		add_nvplist_numeric_fmt(list, -100000, "modes", acptr, 379, "%s :is using modes %s %s",
									acptr->name, get_usermode_string(acptr),
									acptr->user->snomask ? acptr->user->snomask : "");
	}
	return 0;
}
