//=========================================================================
// FILENAME	: playlist.c
// DESCRIPTION	: Playlist
//=========================================================================
// Copyright (c) 2008- NETGEAR, Inc. All Rights Reserved.
//=========================================================================

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "tagutils.h"
#include "log.h"


#define MAX_BUF 4096

#define EXTM3U_STATE_WAIT_NAME 0
#define EXTM3U_STATE_NAME_READ 1
#define EXTM3U_STATE_WAIT_VALUE 2
#define EXTM3U_STATE_UNQUOTED_READ 3
#define EXTM3U_STATE_QUOTE_STARTED 4
#define EXTM3U_STATE_QUOTED_READ 5

#define EXTINF_STATE_WAIT_NAME 0
#define EXTINF_STATE_NAME_READ 1
#define EXTINF_STATE_WAIT_VALUE 2
#define EXTINF_STATE_UNQUOTED_READ 3
#define EXTINF_STATE_QUOTE_STARTED 4
#define EXTINF_STATE_QUOTED_READ 5
#define EXTINF_STATE_WAIT_TITLE 6
#define EXTINF_STATE_TITLE_READ 7

static FILE *fp = 0;
static int _utf8bom = 0;
static int _trackno;

static char _default_track_type[128];
static char _default_track_dlna_extras[128];

static int (*_next_track)(struct song_metadata*, struct stat*, char*, char*);
static int _m3u_next_track(struct song_metadata*, struct stat*, char*, char*);
static int _pls_next_track(struct song_metadata*, struct stat*, char*, char*);

int
fill_m3u_metadata_value(struct song_metadata *psong, char* name, int name_len, char* value, int value_len)
{
	if(strncmp(name, "name", 4) == 0)
	{
		psong->title = (char*)malloc(value_len + 1);
		strncpy(psong->title, value, value_len);
		psong->title[value_len] = '\0';
	}
	else if(strncmp(name, "type", 4) == 0)
		strncpy(_default_track_type, value, value_len);
	else if(strncmp(name, "dlna_extras", 11) == 0)
		strncpy(_default_track_dlna_extras, value, value_len);
	else if(strncmp(name, "logo", 4) == 0)
	{
		// TODO: load playlist logo image
	}

	return 0;
}

int
parse_extm3u(char *extm3u, struct song_metadata *psong)
{
	char *p = extm3u;

	p += 7;
	while(*p && *p == ' ')
		p++;

	int st = EXTM3U_STATE_WAIT_NAME, name_len = 0, value_len = 0;
	char *name = NULL, *value = NULL;

	while (1)
	{
		switch(st)
		{
		case EXTM3U_STATE_WAIT_NAME:
			if(*p != ' ')
			{
				name = p;
				st = EXTM3U_STATE_NAME_READ;
			}
			break;
		case EXTM3U_STATE_NAME_READ:
			if(*p == '=')
			{
				name_len = p - name;
				st = EXTM3U_STATE_WAIT_VALUE;
			}
			else if (*p == ' ')
				st = EXTM3U_STATE_WAIT_NAME;
			break;
		case EXTM3U_STATE_WAIT_VALUE:
			if(*p == '\"')
				st = EXTM3U_STATE_QUOTE_STARTED;
			else
			{
				value = p;
				st = EXTM3U_STATE_UNQUOTED_READ;
			}
			break;
		case EXTM3U_STATE_UNQUOTED_READ:
			if(*p == ' ' || !*p)
			{
				value_len = p - value;
				if (name_len > 0 && value_len > 0)
					fill_m3u_metadata_value(psong, name, name_len, value, value_len);
				name_len = 0;
				value_len = 0;
				st = EXTM3U_STATE_WAIT_NAME;
			}
			break;
		case EXTM3U_STATE_QUOTE_STARTED:
			if(*p == '\"')
			{
				name_len = 0;
				st = EXTM3U_STATE_WAIT_NAME;
			}
			else
			{
				value = p;
				st = EXTM3U_STATE_QUOTED_READ;
			}
			break;
		case EXTM3U_STATE_QUOTED_READ:
			if(*p == '\"')
			{
				value_len = p - value;
				if (name_len > 0 && value_len > 0)
					fill_m3u_metadata_value(psong, name, name_len, value, value_len);
				name_len = 0;
				value_len = 0;
				st = EXTM3U_STATE_WAIT_NAME;
			}
			break;
		}

		if(!*p)
			break;
		p++;
	}

	return 0;
}

int
start_plist(const char *path, struct song_metadata *psong, struct stat *stat, char *lang, char *type)
{
	char *fname, *suffix;

	_next_track = 0;
	_utf8bom = 0;
	_trackno = 0;
	memset((void*)_default_track_type, 0, sizeof(_default_track_type));
	memset((void*)_default_track_dlna_extras, 0, sizeof(_default_track_dlna_extras));

	if(strcasecmp(type, "m3u") == 0)
		_next_track = _m3u_next_track;
	else if(strcasecmp(type, "pls") == 0)
		_next_track = _pls_next_track;

	if(!_next_track)
	{
		DPRINTF(E_ERROR, L_SCANNER, "Unsupported playlist type <%s> (%s)\n", type, path);
		return -1;
	}

	if(!(fp = fopen(path, "rb")))
	{
		DPRINTF(E_ERROR, L_SCANNER, "Cannot open %s\n", path);
		return -1;
	}

	if(!psong)
		return 0;

	memset((void*)psong, 0, sizeof(struct song_metadata));
	psong->is_plist = 1;
	psong->path = strdup(path);
	psong->type = type;

	fname = strrchr(psong->path, '/');
	psong->basename = fname ? fname + 1 : psong->path;

	// search for M3U metadata
	if (strcasecmp(type, "m3u") == 0)
	{
		char buf[MAX_BUF], *p;
		p = fgets(buf, MAX_BUF, fp);

		if (strncmp(p, "#EXTM3U", 7) == 0)
			parse_extm3u(p, psong);

		rewind(fp);
	}

	if (!psong->title)
	{
		psong->title = strdup(psong->basename);
		suffix = strrchr(psong->title, '.');
		if(suffix)
			*suffix = '\0';
	}

	if(stat)
	{
		if(!psong->time_modified)
			psong->time_modified = stat->st_mtime;
		psong->file_size = stat->st_size;
	}

	return 0;
}

int
_m3u_next_track(struct song_metadata *psong, struct stat *stat, char *lang, char *type)
{
	char buf[MAX_BUF], *p;
	int i, len;

	memset((void*)psong, 0, sizeof(struct song_metadata));
	char *track_type = NULL;
	char *track_dlna_extras = NULL;

	// read first line
	p = fgets(buf, MAX_BUF, fp);
	if(!p)
	{
		fclose(fp);
		return 1;
	}

	// check BOM
	if(!_utf8bom && p[0] == '\xef' && p[1] == '\xbb' && p[2] == '\xbf')
	{
		_utf8bom = 1;
		p += 3;
	}

	while(p)
	{
		while(isspace(*p)) p++;

		if(!(*p))
			goto next_line;

		if(*p == '#')
		{
			if(strncmp(p, "#EXTINF:", 8) == 0)
			{
				p += 8; // skip header
				while(*p && (*p != ' ' || *p == ','))// skip duration
					p++;

				// start fsm
				int st = EXTINF_STATE_WAIT_NAME, name_len = 0, value_len = 0, title_len = 0;
				char *name = NULL, *value = NULL, *title = NULL;

				while (1)
				{
					switch(st)
					{
					case EXTINF_STATE_WAIT_NAME:
						if(*p == ',')
							st = EXTINF_STATE_WAIT_TITLE;
						else if(*p != ' ')
						{
							name = p;
							st = EXTINF_STATE_NAME_READ;
						}
						break;
					case EXTINF_STATE_NAME_READ:
						if(*p == '=')
						{
							name_len = p - name;
							st = EXTINF_STATE_WAIT_VALUE;
						}
						else if (*p == ' ')
							st = EXTINF_STATE_WAIT_NAME;
						break;
					case EXTINF_STATE_WAIT_VALUE:
						if(*p == '\"')
							st = EXTINF_STATE_QUOTE_STARTED;
						else
						{
							value = p;
							st = EXTINF_STATE_UNQUOTED_READ;
						}
						break;
					case EXTINF_STATE_UNQUOTED_READ:
						if(*p == ' ' || !*p)
						{
							value_len = p - value;
							if (name_len > 0 && value_len > 0)
							{
								if(strncmp(name, "type", 4) == 0)
								{
									track_type = (char*)malloc(value_len + 1);
									strncpy(track_type, value, value_len);
									track_type[value_len] = '\0';
								}
								else if(strncmp(name, "dlna_extras", 11) == 0)
								{
									track_dlna_extras = (char*)malloc(value_len + 1);
									strncpy(track_dlna_extras, value, value_len);
									track_dlna_extras[value_len] = '\0';
								}
							}
							name_len = 0;
							value_len = 0;
							st = EXTINF_STATE_WAIT_NAME;
						}
						break;
					case EXTINF_STATE_QUOTE_STARTED:
						if(*p == '\"')
						{
							name_len = 0;
							st = EXTINF_STATE_WAIT_NAME;
						}
						else
						{
							value = p;
							st = EXTINF_STATE_QUOTED_READ;
						}
						break;
					case EXTINF_STATE_QUOTED_READ:
						if(*p == '\"')
						{
							value_len = p - value;
							if (name_len > 0 && value_len > 0)
							{
								if(strncmp(name, "type", 4) == 0)
								{
									track_type = (char*)malloc(value_len + 1);
									strncpy(track_type, value, value_len);
									track_type[value_len] = '\0';
								}
								else if(strncmp(name, "dlna_extras", 11) == 0)
								{
									track_dlna_extras = (char*)malloc(value_len + 1);
									strncpy(track_dlna_extras, value, value_len);
									track_dlna_extras[value_len] = '\0';
								}
							}
							name_len = 0;
							value_len = 0;
							st = EXTINF_STATE_WAIT_NAME;
						}
						break;
					case EXTINF_STATE_WAIT_TITLE:
						if(*p != ' ')
						{
							title = p;
							st = EXTINF_STATE_TITLE_READ;
						}
						break;
					case EXTINF_STATE_TITLE_READ:
						if(!*p)
						{
							title_len = p - title;
							if (title_len > 0)
								psong->title = strdup(title);
							title_len = 0;
							st = EXTINF_STATE_WAIT_NAME;
						}
						break;
					}

					if(!*p)
						break;
					p++;
				}
			}

			goto next_line;
		}

		if(!isprint(*p))
		{
			DPRINTF(E_ERROR, L_SCANNER, "Playlist looks bad (unprintable characters)\n");
			fclose(fp);
			return 2;
		}

		psong->track = ++_trackno;

		char *tr_type = track_type ? track_type : _default_track_type;
		if(strlen(tr_type))
		{
			const char *mime_map[] =
				{
					"avi", "video/avi",
					"asf", "video/x-ms-asf",
					"wmv", "video/x-ms-wmv",
					"mp4", "video/mp4",
					"mpeg", "video/mpeg",
					"mpeg_ts", "video/mpeg",
					"mpeg1", "video/mpeg",
					"mpeg2", "video/mpeg2",
					"ts", "video/mp2t",
					"mp2t", "video/mp2t",
					"mp2p", "video/mp2p",
					"mov", "video/quicktime",
					"mkv", "video/x-mkv",
					"3gp", "video/3gpp",
					"flv", "video/x-flv",
					"aac", "audio/x-aac",
					"ac3", "audio/x-ac3",
					"mp3", "audio/mpeg",
					"ogg", "application/ogg",
					"wma", "audio/x-ms-wma",
					NULL
				};
			for(i = 0; mime_map[i]; i += 2)
				if(strcasecmp(tr_type, mime_map[i]) == 0)
				{
					psong->mime = strdup(mime_map[i+1]);
					break;
				}
		}
		if(track_type)
			free(track_type);

		char *dlna_pn = track_dlna_extras ? track_dlna_extras : _default_track_dlna_extras;
		if(strlen(dlna_pn))
		{
			const char *dlna_extras_map[] =
				{
					"mpeg_ps_pal", "MPEG_PS_PAL",
					"mpeg_ps_pal_ac3", "MPEG_PS_PAL_XAC3",
					"mpeg_ps_ntsc", "MPEG_PS_NTSC",
					"mpeg_ps_ntsc_ac3", "MPEG_PS_NTSC_XAC3",
					"mpeg1", "MPEG1",
					"mpeg_ts_sd", "MPEG_TS_SD_NA_ISO",
					"mpeg_ts_hd", "MPEG_TS_HD_NA",
					"avchd", "AVC_TS_HD_50_AC3",
					"wmv_med_base", "WMVMED_BASE",
					"wmv_med_full", "WMVMED_FULL",
					"wmv_med_pro", "WMVMED_PRO",
					"wmv_high_full", "WMVHIGH_FULL",
					"wmv_high_pro", "WMVHIGH_PRO",
					"asf_mpeg4_sp", "MPEG4_P2_ASF_SP_G726",
					"asf_mpeg4_asp_l4", "MPEG4_P2_ASF_ASP_L4_SO_G726",
					"asf_mpeg4_asp_l5", "MPEG4_P2_ASF_ASP_L5_SO_G726",
					"asf_vc1_l1", "VC1_ASF_AP_L1_WMA",
					"mp4_avc_sd_mp3", "AVC_MP4_MP_SD_MPEG1_L3",
					"mp4_avc_sd_ac3", "AVC_MP4_MP_SD_AC3",
					"mp4_avc_hd_ac3", "AVC_MP4_MP_HD_AC3",
					"mp4_avc_sd_aac", "AVC_MP4_MP_SD_AAC_MULT5",
					"mpeg_ts_hd_mp3", "AVC_TS_MP_HD_MPEG1_L3",
					"mpeg_ts_hd_ac3", "AVC_TS_MP_HD_AC3",
					"mpeg_ts_mpeg4_asp_mp3", "MPEG4_P2_TS_ASP_MPEG1_L3",
					"mpeg_ts_mpeg4_asp_ac3", "MPEG4_P2_TS_ASP_AC3",
					"avi", "AVI",
					"divx5", "PV_DIVX_DX50",
					"mp3", "MP3",
					"ac3", "AC3",
					"wma_base", "WMABASE",
					"wma_full", "WMAFULL",
					"wma_pro", "WMAPRO",
					NULL
				};
			for(i = 0; dlna_extras_map[i]; i += 2)
				if(strcasecmp(dlna_pn, dlna_extras_map[i]) == 0)
				{
					psong->dlna_pn = strdup(dlna_extras_map[i+1]);
					break;
				}
		}
		if(track_dlna_extras)
			free(track_dlna_extras);

		len = strlen(p);
		while(p[len-1] == '\r' || p[len-1] == '\n')
			p[--len] = '\0';
		psong->path = strdup(p);
		return 0;
next_line:
		p = fgets(buf, MAX_BUF, fp);
	}

	fclose(fp);
	return 1;
}

int
_pls_next_track(struct song_metadata *psong, struct stat *stat, char *lang, char *type)
{
	char buf[MAX_BUF], *p;
	int len;

	memset((void*)psong, 0, sizeof(struct song_metadata));

	// read first line
	p = fgets(buf, MAX_BUF, fp);
	if(!p)
	{
		fclose(fp);
		return 1;
	}

	while(p)
	{
		while(isspace(*p)) p++;

		if(!(*p) || *p == '#')
			goto next_line;

		if(!isprint(*p))
		{
			DPRINTF(E_ERROR, L_SCANNER, "Playlist looks bad (unprintable characters)\n");
			fclose(fp);
			return 2;
		}

		// verify that it's a valid pls playlist
		if(!_trackno)
		{
			if(strncmp(p, "[playlist]", 10))
				break;
			_trackno++;
			goto next_line;
		}

		if(strncmp(p, "File", 4) != 0)
			goto next_line;

		psong->track = strtol(p+4, &p, 10);
		if(!psong->track || !p++)
			goto next_line;
		_trackno = psong->track;

		len = strlen(p);
		while(p[len-1] == '\r' || p[len-1] == '\n')
			p[--len] = '\0';
		psong->path = strdup(p);
		return 0;
next_line:
		p = fgets(buf, MAX_BUF, fp);
	}

	fclose(fp);
	return 1;
}

int
next_plist_track(struct song_metadata *psong, struct stat *stat, char *lang, char *type)
{
	if(_next_track)
		return _next_track(psong, stat, lang, type);
	return -1;
}
