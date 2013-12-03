/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wtperf.h"

/* All options changeable on command line using -o or -O are listed here. */
static CONFIG_OPT config_opts[] = {
#define	OPT_DEFINE_DESC
#include "wtperf_opt.i"
#undef OPT_DEFINE_DESC
};

static int  config_opt(CONFIG *, WT_CONFIG_ITEM *, WT_CONFIG_ITEM *);
static void config_opt_usage(void);

/*
 * STRING_MATCH --
 *	Return if a string matches a bytestring of a specified length.
 */
#undef	STRING_MATCH
#define	STRING_MATCH(str, bytes, len)					\
	(strncmp(str, bytes, len) == 0 && (str)[(len)] == '\0')


/* Assign the src config to the dest.
 * Any storage allocated in dest is freed as a result.
 */
int
config_assign(CONFIG *dest, const CONFIG *src)
{
	size_t i, len;
	char *newstr, **pstr;

	config_free(dest);
	memcpy(dest, src, sizeof(CONFIG));

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE ||
		    config_opts[i].type == CONFIG_STRING_TYPE) {
			pstr = (char **)
			    ((u_char *)dest + config_opts[i].offset);
			if (*pstr != NULL) {
				len = strlen(*pstr) + 1;
				if ((newstr = malloc(len)) == NULL)
					return (enomem(src));
				strncpy(newstr, *pstr, len);
				*pstr = newstr;
			}
		}
	return (0);
}

/* Free any storage allocated in the config struct.
 */
void
config_free(CONFIG *cfg)
{
	size_t i;
	char **pstr;

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE ||
		    config_opts[i].type == CONFIG_STRING_TYPE) {
			pstr = (char **)
			    ((unsigned char *)cfg + config_opts[i].offset);
			if (*pstr != NULL) {
				free(*pstr);
				*pstr = NULL;
			}
		}

	free(cfg->uri);
}

/*
 * config_threads --
 *	Parse the thread configuration.
 */
static int
config_threads(CONFIG *cfg, const char *config, size_t len)
{
	WORKLOAD *workp;
	WT_CONFIG_ITEM groupk, groupv, k, v;
	WT_CONFIG_SCAN *group, *scan;
	WT_EXTENSION_API *wt_api;
	int ret;

	wt_api = cfg->conn->get_extension_api(cfg->conn);

	/* Allocate the workload array. */
	if ((cfg->workload = calloc(WORKLOAD_MAX, sizeof(WORKLOAD))) == NULL)
		return (enomem(cfg));
	cfg->workload_cnt = 0;

	/*
	 * The thread configuration may be in multiple groups, that is, we have
	 * to handle configurations like:
	 *	threads=((count=2,reads=1),(count=8,inserts=2,updates=1))
	 *
	 * Start a scan on the original string, then do scans on each string
	 * returned from the original string.
	 */
	if ((ret =
	    wt_api->config_scan_begin(wt_api, NULL, config, len, &group)) != 0)
		goto err;
	while ((ret =
	    wt_api->config_scan_next(wt_api, group, &groupk, &groupv)) == 0) {
		if ((ret = wt_api-> config_scan_begin(
		    wt_api, NULL, groupk.str, groupk.len, &scan)) != 0)
			goto err;
		
		/* Move to the next workload slot. */
		if (cfg->workload_cnt == WORKLOAD_MAX) {
			fprintf(stderr,
			    "too many workloads configured, only %d workloads "
			    "supported\n",
			    WORKLOAD_MAX);
			return (EINVAL);
		}
		workp = &cfg->workload[cfg->workload_cnt++];

		while ((ret =
		    wt_api->config_scan_next(wt_api, scan, &k, &v)) == 0) {
			if (STRING_MATCH("count", k.str, k.len)) {
				if ((workp->threads = v.val) <= 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("insert", k.str, k.len) ||
			    STRING_MATCH("inserts", k.str, k.len)) {
				if ((workp->insert = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("read", k.str, k.len) ||
			    STRING_MATCH("reads", k.str, k.len)) {
				if ((workp->read = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("update", k.str, k.len) ||
			    STRING_MATCH("updates", k.str, k.len)) {
				if ((workp->update = v.val) < 0)
					goto err;
				continue;
			}
			goto err;
		}
		if (ret == WT_NOTFOUND)
			ret = 0;
		if (ret != 0 )
			goto err;
		if ((ret = wt_api->config_scan_end(wt_api, scan)) != 0)
			goto err;

		if (workp->insert == 0 &&
		    workp->read == 0 && workp->update == 0)
			goto err;
		cfg->workers_cnt += (u_int)workp->threads;
	}

	if ((ret = wt_api->config_scan_end(wt_api, group)) != 0)
		goto err;

	return (0);

err:	fprintf(stderr,
	    "invalid thread configuration or scan error: %.*s\n",
	    (int)len, config);
	return (EINVAL);
}

/*
 * Check a single key=value returned by the config parser
 * against our table of valid keys, along with the expected type.
 * If everything is okay, set the value.
 */
static int
config_opt(CONFIG *cfg, WT_CONFIG_ITEM *k, WT_CONFIG_ITEM *v)
{
	CONFIG_OPT *popt;
	char *newstr, **strp;
	size_t i, nopt;
	uint64_t newlen;
	void *valueloc;

	popt = NULL;
	nopt = sizeof(config_opts)/sizeof(config_opts[0]);
	for (i = 0; i < nopt; i++)
		if (strlen(config_opts[i].name) == k->len &&
		    strncmp(config_opts[i].name, k->str, k->len) == 0) {
			popt = &config_opts[i];
			break;
		}
	if (popt == NULL) {
		fprintf(stderr, "wtperf: Error: "
		    "unknown option \'%.*s\'\n", (int)k->len, k->str);
		fprintf(stderr, "Options:\n");
		for (i = 0; i < nopt; i++)
			fprintf(stderr, "\t%s\n", config_opts[i].name);
		return (EINVAL);
	}
	valueloc = ((unsigned char *)cfg + popt->offset);
	switch (popt->type) {
	case BOOL_TYPE:
		if (v->type != WT_CONFIG_ITEM_BOOL) {
			fprintf(stderr, "wtperf: Error: "
			    "bad bool value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case INT_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad int value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val > INT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "int value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case UINT32_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad uint32 value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val < 0 || v->val > UINT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "uint32 value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(uint32_t *)valueloc = (uint32_t)v->val;
		break;
	case CONFIG_STRING_TYPE:
		if (v->type != WT_CONFIG_ITEM_STRING) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		newlen = v->len + 1;
		if (*strp == NULL) {
			if ((newstr = calloc(newlen, sizeof(char))) == NULL)
				return (enomem(cfg));
			strncpy(newstr, v->str, v->len);
		} else {
			newlen += (strlen(*strp) + 1);
			if ((newstr = calloc(newlen, sizeof(char))) == NULL)
				return (enomem(cfg));
			snprintf(newstr, newlen,
			    "%s,%*s", *strp, (int)v->len, v->str);
			/* Free the old value now we've copied it. */
			free(*strp);
		}
		*strp = newstr;
		break;
	case STRING_TYPE:
		/*
		 * Thread configuration is the one case where the type isn't a
		 * "string", it's a "struct".
		 */
		if (v->type == WT_CONFIG_ITEM_STRUCT &&
		    STRING_MATCH("threads", k->str, k->len))
			return (config_threads(cfg, v->str, v->len));

		if (v->type != WT_CONFIG_ITEM_STRING) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		free(*strp);
		if ((newstr = malloc(v->len + 1)) == NULL)
			return (enomem(cfg));
		strncpy(newstr, v->str, v->len);
		newstr[v->len] = '\0';
		*strp = newstr;
		break;
	}
	return (0);
}

/* Parse a configuration file.
 * We recognize comments '#' and continuation via lines ending in '\'.
 */
int
config_opt_file(CONFIG *cfg, const char *filename)
{
	FILE *fp;
	size_t linelen, optionpos;
	int contline, linenum, ret;
	char line[256], option[1024];
	char *comment, *ltrim, *rtrim;

	if ((fp = fopen(filename, "r")) == NULL) {
		fprintf(stderr, "wtperf: %s: %s\n", filename, strerror(errno));
		return (errno);
	}

	ret = 0;
	optionpos = 0;
	linenum = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		linenum++;
		/* trim the line */
		for (ltrim = line; *ltrim && isspace(*ltrim); ltrim++)
			;
		rtrim = &ltrim[strlen(ltrim)];
		if (rtrim > ltrim && rtrim[-1] == '\n')
			rtrim--;

		contline = (rtrim > ltrim && rtrim[-1] == '\\');
		if (contline)
			rtrim--;

		comment = strchr(ltrim, '#');
		if (comment != NULL && comment < rtrim)
			rtrim = comment;
		while (rtrim > ltrim && isspace(rtrim[-1]))
			rtrim--;

		linelen = (size_t)(rtrim - ltrim);
		if (linelen == 0)
			continue;

		if (linelen + optionpos + 1 > sizeof(option)) {
			fprintf(stderr, "wtperf: %s: %d: line overflow\n",
			    filename, linenum);
			ret = EINVAL;
			break;
		}
		*rtrim = '\0';
		strncpy(&option[optionpos], ltrim, linelen);
		option[optionpos + linelen] = '\0';
		if (contline)
			optionpos += linelen;
		else {
			if ((ret = config_opt_line(cfg, option)) != 0) {
				fprintf(stderr, "wtperf: %s: %d: parse error\n",
				    filename, linenum);
				break;
			}
			optionpos = 0;
		}
	}
	if (ret == 0 && optionpos > 0) {
		fprintf(stderr, "wtperf: %s: %d: last line continues\n",
		    filename, linenum);
		ret = EINVAL;
	}

	(void)fclose(fp);
	return (ret);
}

/* Parse a single line of config options.
 * Continued lines have already been joined.
 */
int
config_opt_line(CONFIG *cfg, const char *optstr)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_SCAN *scan;
	WT_EXTENSION_API *wt_api;
	int ret, t_ret;

	wt_api = cfg->conn->get_extension_api(cfg->conn);

	if ((ret = wt_api->config_scan_begin(
	    wt_api, NULL, optstr, strlen(optstr), &scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_begin");
		return (ret);
	}

	while (ret == 0) {
		if ((ret =
		    wt_api->config_scan_next(wt_api, scan, &k, &v)) != 0) {
			/* Any parse error has already been reported. */
			if (ret == WT_NOTFOUND)
				ret = 0;
			break;
		}
		ret = config_opt(cfg, &k, &v);
	}
	if ((t_ret = wt_api->config_scan_end(wt_api, scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_end");
		if (ret == 0)
			ret = t_ret;
	}

	return (ret);
}

/* Set a single string config option */
int
config_opt_str(CONFIG *cfg, const char *name, const char *value)
{
	int ret;
	char *optstr;

							/* name="value" */
	if ((optstr = malloc(strlen(name) + strlen(value) + 4)) == NULL)
		return (enomem(cfg));
	sprintf(optstr, "%s=\"%s\"", name, value);
	ret = config_opt_line(cfg, optstr);
	free(optstr);
	return (ret);
}

static void
pretty_print(const char *p, const char *indent)
{
	const char *t;

	for (;; p = t + 1) {
		if (strlen(p) <= 70)
			break;
		for (t = p + 70; t > p && *t != ' '; --t)
			;
		if (t == p)			/* No spaces? */
			break;
		printf("%s%.*s\n",
		    indent == NULL ? "" : indent, (int)(t - p), p);
	}
	if (*p != '\0')
		printf("%s%s\n", indent == NULL ? "" : indent, p);
}

static void
config_opt_usage(void)
{
	size_t i, nopt;
	const char *defaultval, *typestr;

	pretty_print(
	    "The following are options settable using -o or -O, showing the "
	    "type and default value.\n", NULL);
	pretty_print(
	    "String values must be enclosed in \" quotes, boolean values must "
	    "be either true or false.\n", NULL);

	nopt = sizeof(config_opts)/sizeof(config_opts[0]);
	for (i = 0; i < nopt; i++) {
		typestr = "?";
		defaultval = config_opts[i].defaultval;
		switch (config_opts[i].type) {
		case BOOL_TYPE:
			typestr = "boolean";
			if (strcmp(defaultval, "0") == 0)
				defaultval = "false";
			else
				defaultval = "true";
			break;
		case CONFIG_STRING_TYPE:
		case STRING_TYPE:
			typestr = "string";
			break;
		case INT_TYPE:
			typestr = "int";
			break;
		case UINT32_TYPE:
			typestr = "unsigned int";
			break;
		}
		printf("%s (%s, default=%s)\n",
		    config_opts[i].name, typestr, defaultval);
		pretty_print(config_opts[i].description, "\t");
	}
}

int
config_sanity(CONFIG *cfg)
{
	/* Various intervals should be less than the run-time. */
	if (cfg->run_time > 0 &&
	    ((cfg->checkpoint_threads != 0 &&
	    cfg->checkpoint_interval > cfg->run_time) ||
	    cfg->report_interval > cfg->run_time ||
	    cfg->sample_interval > cfg->run_time)) {
		fprintf(stderr, "interval value longer than the run-time\n");
		return (1);
	}
	return (0);
}

void
config_print(CONFIG *cfg)
{
	WORKLOAD *workp;
	u_int i;

	printf("Workload configuration:\n");
	printf("\tHome: %s\n", cfg->home);
	printf("\tTable name: %s\n", cfg->table_name);
	printf("\tConnection configuration: %s\n", cfg->conn_config);

	printf("\t%s table: %s\n",
	    cfg->create ? "Creating new" : "Using existing",
	    cfg->table_config);
	printf("\tKey size: %" PRIu32 ", value size: %" PRIu32 "\n",
	    cfg->key_sz, cfg->value_sz);
	if (cfg->create)
		printf("\tPopulate threads: %" PRIu32 ", inserting %" PRIu32
		    " rows\n",
		    cfg->populate_threads, cfg->icount);

	printf("\tWorkload seconds, operations: %" PRIu32 ", %" PRIu32 "\n",
	    cfg->run_time, cfg->run_ops);
	if (cfg->workload != NULL) {
		printf("\tWorkload configuration(s):\n");
		for (i = 0, workp = cfg->workload;
		    i < cfg->workload_cnt; ++i, ++workp)
			printf("\t\t%" PRId64 " threads (inserts=%" PRId64
			    ", reads=%" PRId64 ", updates=%" PRId64 ")\n",
			    workp->threads,
			    workp->insert, workp->read, workp->update);
	}

	printf("\tCheckpoint threads, interval: %" PRIu32 ", %" PRIu32 "\n",
	    cfg->checkpoint_threads, cfg->checkpoint_interval);
	printf("\tReporting interval: %" PRIu32 "\n", cfg->report_interval);
	printf("\tSampling interval: %" PRIu32 "\n", cfg->sample_interval);

	printf("\tVerbosity: %" PRIu32 "\n", cfg->verbose);
}

void
usage(void)
{
	printf("wtperf [-LMSv] [-C config] "
	    "[-h home] [-O file] [-o option] [-T config]\n");
	printf("\t-L Use a large default configuration\n");
	printf("\t-M Use a medium default configuration\n");
	printf("\t-S Use a small default configuration\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t            (added to option conn_config)\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST\n");
	printf("\t-O <file> file contains options as listed below\n");
	printf("\t-o option=val[,option=val,...] set options listed below\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t            (added to option table_config)\n");
	printf("\n");
	config_opt_usage();
}