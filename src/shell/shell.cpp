/*
 *  Copyright (C) 2002-2019  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA.
 */

#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "dosbox.h"
#include "regs.h"
#include "control.h"
#include "shell.h"
#include "menu.h"
#include "cpu.h"
#include "callback.h"
#include "support.h"
#include "builtin.h"
#include "mapper.h"
#include <unistd.h>
#include <time.h>
#include "build_timestamp.h"

extern bool enable_config_as_shell_commands;
extern bool dos_shell_running_program;
bool usecon = true;

Bit16u shell_psp = 0;

Bitu call_int2e = 0;

void MSG_Replace(const char * _name, const char* _val);

void CALLBACK_DeAllocate(Bitu in);

Bitu call_shellstop = 0;
/* Larger scope so shell_del autoexec can use it to
 * remove things from the environment */
DOS_Shell * first_shell = 0; 

static Bitu shellstop_handler(void) {
	return CBRET_STOP;
}

static void SHELL_ProgramStart(Program * * make) {
	*make = new DOS_Shell;
}
//Repeat it with the correct type, could do it in the function below, but this way it should be 
//clear that if the above function is changed, this function might need a change as well.
static void SHELL_ProgramStart_First_shell(DOS_Shell * * make) {
	*make = new DOS_Shell;
}

#define AUTOEXEC_SIZE 4096
static char autoexec_data[AUTOEXEC_SIZE] = { 0 };
static std::list<std::string> autoexec_strings;
typedef std::list<std::string>::iterator auto_it;

void VFILE_Remove(const char *name);

void AutoexecObject::Install(const std::string &in) {
	if(GCC_UNLIKELY(installed)) E_Exit("autoexec: already created %s",buf.c_str());
	installed = true;
	buf = in;
	autoexec_strings.push_back(buf);
	this->CreateAutoexec();

	//autoexec.bat is normally created AUTOEXEC_Init.
	//But if we are already running (first_shell)
	//we have to update the envirionment to display changes

	if(first_shell)	{
		//create a copy as the string will be modified
		std::string::size_type n = buf.size();
		char* buf2 = new char[n + 1];
		safe_strncpy(buf2, buf.c_str(), n + 1);
		if((strncasecmp(buf2,"set ",4) == 0) && (strlen(buf2) > 4)){
			char* after_set = buf2 + 4;//move to variable that is being set
			char* test2 = strpbrk(after_set,"=");
			if(!test2) {first_shell->SetEnv(after_set,"");return;}
			*test2++ = 0;
			//If the shell is running/exists update the environment
			first_shell->SetEnv(after_set,test2);
		}
		delete [] buf2;
	}
}

void AutoexecObject::InstallBefore(const std::string &in) {
	if(GCC_UNLIKELY(installed)) E_Exit("autoexec: already created %s",buf.c_str());
	installed = true;
	buf = in;
	autoexec_strings.push_front(buf);
	this->CreateAutoexec();
}

void AutoexecObject::CreateAutoexec(void) {
	/* Remove old autoexec.bat if the shell exists */
	if(first_shell)	VFILE_Remove("AUTOEXEC.BAT");

	//Create a new autoexec.bat
	autoexec_data[0] = 0;
	size_t auto_len;
	for(auto_it it = autoexec_strings.begin(); it != autoexec_strings.end(); ++it) {

		std::string linecopy = *it;
		std::string::size_type offset = 0;
		//Lets have \r\n as line ends in autoexec.bat.
		while(offset < linecopy.length()) {
			std::string::size_type  n = linecopy.find("\n",offset);
			if ( n == std::string::npos ) break;
			std::string::size_type rn = linecopy.find("\r\n",offset);
			if ( rn != std::string::npos && rn + 1 == n) {offset = n + 1; continue;}
			// \n found without matching \r
			linecopy.replace(n,1,"\r\n");
			offset = n + 2;
		}

		auto_len = strlen(autoexec_data);
		if ((auto_len+linecopy.length() + 3) > AUTOEXEC_SIZE) {
			E_Exit("SYSTEM:Autoexec.bat file overflow");
		}
		sprintf((autoexec_data + auto_len),"%s\r\n",linecopy.c_str());
	}
	if (first_shell) VFILE_Register("AUTOEXEC.BAT",(Bit8u *)autoexec_data,(Bit32u)strlen(autoexec_data));
}

void AutoexecObject::Uninstall() {
	if(!installed) return;

	// Remove the line from the autoexecbuffer and update environment
	for(auto_it it = autoexec_strings.begin(); it != autoexec_strings.end(); ) {
		if ((*it) == buf) {
			std::string::size_type n = buf.size();
			char* buf2 = new char[n + 1];
			safe_strncpy(buf2, buf.c_str(), n + 1);
			bool stringset = false;
			// If it's a environment variable remove it from there as well
			if ((strncasecmp(buf2,"set ",4) == 0) && (strlen(buf2) > 4)){
				char* after_set = buf2 + 4;//move to variable that is being set
				char* test2 = strpbrk(after_set,"=");
				if (!test2) continue;
				*test2 = 0;
				stringset = true;
				//If the shell is running/exists update the environment
				if (first_shell) first_shell->SetEnv(after_set,"");
			}
			delete [] buf2;
			if (stringset && first_shell && first_shell->bf && first_shell->bf->filename.find("AUTOEXEC.BAT") != std::string::npos) {
				//Replace entry with spaces if it is a set and from autoexec.bat, as else the location counter will be off.
				*it = buf.assign(buf.size(),' ');
				++it;
			} else {
				it = autoexec_strings.erase(it);
			}
		} else ++it;
	}
	installed=false;
	this->CreateAutoexec();
}

AutoexecObject::~AutoexecObject(){
	Uninstall();
}

DOS_Shell::~DOS_Shell() {
	if (bf != NULL) delete bf; /* free batch file */
}

DOS_Shell::DOS_Shell():Program(){
	input_handle=STDIN;
	echo=true;
	exit=false;
	bf=0;
	call=false;
	lfnfor = uselfn;
    input_eof=false;
    completion_index = 0;
}

Bitu DOS_Shell::GetRedirection(char *s, char **ifn, char **ofn, char **toc,bool * append) {

	char * lr=s;
	char * lw=s;
	char ch;
	Bitu num=0;
	bool quote = false;
	char* t;
	int q;

	while ( (ch=*lr++) ) {
		if(quote && ch != '"') { /* don't parse redirection within quotes. Not perfect yet. Escaped quotes will mess the count up */
			*lw++ = ch;
			continue;
		}

		switch (ch) {
		case '"':
			quote = !quote;
			break;
		case '>':
			*append = ((*lr) == '>');
			if (*append)
				lr++;
			lr = ltrim(lr);
			if (*ofn)
				free(*ofn);
			*ofn = lr;
			q = 0;
			while (*lr && (q/2*2!=q || *lr != ' ') && *lr != '<' && *lr != '|') {
				if (*lr=='"')
					q++;
				lr++;
			}
			// if it ends on a : => remove it.
			if ((*ofn != lr) && (lr[-1] == ':'))
				lr[-1] = 0;
			t = (char*)malloc(lr-*ofn+1);
			safe_strncpy(t, *ofn, lr-*ofn+1);
			*ofn = t;
			continue;
		case '<':
			if (*ifn)
				free(*ifn);
			lr = ltrim(lr);
			*ifn = lr;
			q = 0;
			while (*lr && (q/2*2!=q || *lr != ' ') && *lr != '>' && *lr != '|') {
				if (*lr=='"')
					q++;
				lr++;
			}
			if ((*ifn != lr) && (lr[-1] == ':'))
				lr[-1] = 0;
			t = (char*)malloc(lr-*ifn+1);
			safe_strncpy(t, *ifn, lr-*ifn+1);
			*ifn = t;
			continue;
		case '|':
			num++;
			if (*toc)
				free(*toc);
			lr = ltrim(lr);
			*toc = lr;
			while (*lr)
				lr++;
			t = (char*)malloc(lr-*toc+1);
			safe_strncpy(t, *toc, lr-*toc+1);
			*toc = t;
			continue;
		}
		*lw++=ch;
	}
	*lw=0;
	return num;
}	

void DOS_Shell::ParseLine(char * line) {
	LOG(LOG_EXEC,LOG_DEBUG)("Parsing command line: %s",line);
	/* Check for a leading @ */
 	if (line[0] == '@') line[0] = ' ';
	line = trim(line);

	/* Do redirection and pipe checks */
	
	char * in  = 0;
	char * out = 0;
	char * toc = 0;

	Bit16u dummy,dummy2;
	Bit32u bigdummy = 0;
	Bitu num = 0;		/* Number of commands in this line */
	bool append;
	bool normalstdin  = false;	/* wether stdin/out are open on start. */
	bool normalstdout = false;	/* Bug: Assumed is they are "con"      */
	
	num = GetRedirection(line,&in, &out, &toc, &append);
	if (in || out || toc) {
		normalstdin  = (psp->GetFileHandle(0) != 0xff); 
		normalstdout = (psp->GetFileHandle(1) != 0xff); 
	}
	if (in) {
		if(DOS_OpenFile(in,OPEN_READ,&dummy)) {	//Test if file exists
			DOS_CloseFile(dummy);
			LOG_MSG("SHELL:Redirect input from %s",in);
			if(normalstdin) DOS_CloseFile(0);	//Close stdin
			DOS_OpenFile(in,OPEN_READ,&dummy);	//Open new stdin
		} else
			WriteOut(!*in?"File open error\n":(dos.errorcode==DOSERR_ACCESS_DENIED?"Access denied - %s\n":"File open error - %s\n"), in);
	}
	bool fail=false;
	char pipetmp[270];
	Bit16u fattr;
	if (toc) {
#ifdef WIN32
		srand(GetTickCount());
#else
		struct timespec ts;
		unsigned theTick = 0U;
		clock_gettime( CLOCK_REALTIME, &ts );
		theTick  = ts.tv_nsec / 1000000;
		theTick += ts.tv_sec * 1000;
		srand(theTick);
#endif
		std::string line;
		if (!GetEnvStr("TEMP",line)&&!GetEnvStr("TMP",line))
			sprintf(pipetmp, "pipe%d.tmp", rand()%10000);
		else {
			std::string::size_type idx = line.find('=');
			std::string temp=line.substr(idx +1 , std::string::npos);
			if (DOS_GetFileAttr(temp.c_str(), &fattr) && fattr&DOS_ATTR_DIRECTORY)
				sprintf(pipetmp, "%s\\pipe%d.tmp", temp.c_str(), rand()%10000);
			else
				sprintf(pipetmp, "pipe%d.tmp", rand()%10000);
		}
	}
	if (out||toc) {
		if (out&&toc)
			WriteOut(!*out?"Duplicate redirection\n":"Duplicate redirection - %s\n", out);
		LOG_MSG("SHELL:Redirect output to %s",toc?pipetmp:out);
		if(normalstdout) DOS_CloseFile(1);
		if(!normalstdin && !in) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		bool status = true;
		/* Create if not exist. Open if exist. Both in read/write mode */
		if(!toc&&append) {
			if (DOS_GetFileAttr(out, &fattr) && fattr&DOS_ATTR_READ_ONLY) {
				DOS_SetError(DOSERR_ACCESS_DENIED);
				status = false;
			} else if( (status = DOS_OpenFile(out,OPEN_READWRITE,&dummy)) ) {
				 DOS_SeekFile(1,&bigdummy,DOS_SEEK_END);
			} else {
				status = DOS_CreateFile(out,DOS_ATTR_ARCHIVE,&dummy);	//Create if not exists.
			}
		} else if (!toc&&DOS_GetFileAttr(out, &fattr) && fattr&DOS_ATTR_READ_ONLY) {
			DOS_SetError(DOSERR_ACCESS_DENIED);
			status = false;
		} else {
			if (toc&&DOS_FindFirst(pipetmp, ~DOS_ATTR_VOLUME)&&!DOS_UnlinkFile(pipetmp))
				fail=true;
			status = DOS_OpenFileExtended(toc?pipetmp:out,OPEN_READWRITE,DOS_ATTR_ARCHIVE,0x12,&dummy,&dummy2);
		}
		
		if(!status && normalstdout) {
			DOS_OpenFile("con", OPEN_READWRITE, &dummy);							// Read only file, open con again
			if (!toc) {
				WriteOut(!*out?"File creation error\n":(dos.errorcode==DOSERR_ACCESS_DENIED?"Access denied - %s\n":"File creation error - %s\n"), out);
				DOS_CloseFile(1);
				DOS_OpenFile("nul", OPEN_READWRITE, &dummy);
			}
		}
		if(!normalstdin && !in) DOS_CloseFile(0);
	}
	/* Run the actual command */

	if (this == first_shell) dos_shell_running_program = true;
#if defined(WIN32) && !defined(C_SDL2)
	int Reflect_Menu(void);
	Reflect_Menu();
#endif
	if (toc||(!toc&&((out&&DOS_FindDevice(out)!=DOS_FindDevice("con"))||(in&&DOS_FindDevice(in)!=DOS_FindDevice("con"))))) usecon=false;

	DoCommand(line);

	if (this == first_shell) dos_shell_running_program = false;
#if defined(WIN32) && !defined(C_SDL2)
	int Reflect_Menu(void);
	Reflect_Menu();
#endif

	/* Restore handles */
	if(in) {
		DOS_CloseFile(0);
		if(normalstdin) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		free(in);
	}
	if(out||toc) {
		DOS_CloseFile(1);
		if(!normalstdin) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		if(normalstdout) DOS_OpenFile("con",OPEN_READWRITE,&dummy);
		if(!normalstdin) DOS_CloseFile(0);
		if (out) free(out);
	}
	if (toc)
		{
		if (!fail&&DOS_OpenFile(pipetmp, OPEN_READ, &dummy))					// Test if file can be opened for reading
			{
			DOS_CloseFile(dummy);
			if (normalstdin)
				DOS_CloseFile(0);												// Close stdin
			DOS_OpenFile(pipetmp, OPEN_READ, &dummy);							// Open new stdin
			ParseLine(toc);
			DOS_CloseFile(0);
			if (normalstdin)
				DOS_OpenFile("con", OPEN_READWRITE, &dummy);
			}
		else
			WriteOut("\nFailed to create/open a temporary file for piping. Check the %%TEMP%% variable.\n");
		free(toc);
		if (DOS_FindFirst(pipetmp, ~DOS_ATTR_VOLUME)) DOS_UnlinkFile(pipetmp);
		}
	usecon=true;
}



void DOS_Shell::RunInternal(void) {
	char input_line[CMD_MAXLINE] = {0};
	while (bf) {
		if (bf->ReadLine(input_line)) {
			if (echo) {
				if (input_line[0] != '@') {
					ShowPrompt();
					WriteOut_NoParsing(input_line);
					WriteOut_NoParsing("\n");
				}
			}
			ParseLine(input_line);
			if (echo) WriteOut_NoParsing("\n");
		}
	}
}

void DOS_Shell::Run(void) {
	char input_line[CMD_MAXLINE] = {0};
	std::string line;
	bool optC=cmd->FindStringRemainBegin("/C",line), optK=false;
	if (!optC) optK=cmd->FindStringRemainBegin("/K",line);	
	if (optC||optK) {
		input_line[CMD_MAXLINE-1u] = 0;
		strncpy(input_line,line.c_str(),CMD_MAXLINE-1u);
		char* sep = strpbrk(input_line,"\r\n"); //GTA installer
		if (sep) *sep = 0;
		DOS_Shell temp;
		temp.echo = echo;
		temp.ParseLine(input_line);		//for *.exe *.com  |*.bat creates the bf needed by runinternal;
		temp.RunInternal();				// exits when no bf is found.
		if (!optK||temp.exit)
			return;
	} else if (cmd->FindStringRemain("/?",line)) {
		WriteOut(MSG_Get("SHELL_CMD_COMMAND_HELP"));
		return;
	}

    if (this == first_shell) {
        /* Start a normal shell and check for a first command init */
        WriteOut(MSG_Get("SHELL_STARTUP_BEGIN"),VERSION,SDL_STRING,UPDATED_STR);
        WriteOut(MSG_Get("SHELL_STARTUP_BEGIN2"));
        WriteOut(MSG_Get("SHELL_STARTUP_BEGIN3"));
#if C_DEBUG
        WriteOut(MSG_Get("SHELL_STARTUP_DEBUG"));
#endif
        if (machine == MCH_CGA || machine == MCH_AMSTRAD) WriteOut(MSG_Get("SHELL_STARTUP_CGA"));
        if (machine == MCH_PC98) WriteOut(MSG_Get("SHELL_STARTUP_PC98"));
        if (machine == MCH_HERC || machine == MCH_MDA) WriteOut(MSG_Get("SHELL_STARTUP_HERC"));
        WriteOut(MSG_Get("SHELL_STARTUP_END"));
    }
    else {
        WriteOut(optK?"\n":"DOSBox-X command shell %s %s\n\n",VERSION,UPDATED_STR);
    }

	if (cmd->FindString("/INIT",line,true)) {
		input_line[CMD_MAXLINE-1u] = 0;
		strncpy(input_line,line.c_str(),CMD_MAXLINE-1u);
		line.erase();
		ParseLine(input_line);
	}
	do {
		/* Get command once a line */
		if (bf) {
			if (bf->ReadLine(input_line)) {
				if (echo) {
					if (input_line[0]!='@') {
						ShowPrompt();
						WriteOut_NoParsing(input_line);
						WriteOut_NoParsing("\n");
					}
				}
			} else input_line[0]='\0';
		} else {
			if (echo) ShowPrompt();
			InputCommand(input_line);
			if (echo && !input_eof) WriteOut("\n");

            /* Bugfix: CTTY NUL will return immediately, the shell input will return
             *         immediately, and if we don't consume CPU cycles to compensate,
             *         will leave DOSBox-X running in an endless loop, hung. */
            if (input_eof) CALLBACK_Idle();
		}

		/* do it */
		if(strlen(input_line)!=0) {
			ParseLine(input_line);
			if (echo && !bf) WriteOut_NoParsing("\n");
		}
	} while (!exit);
}

void DOS_Shell::SyntaxError(void) {
	WriteOut(MSG_Get("SHELL_SYNTAXERROR"));
}

class AUTOEXEC:public Module_base {
private:
	AutoexecObject autoexec[17];
	AutoexecObject autoexec_echo;
    AutoexecObject autoexec_auto_bat;
public:
	AUTOEXEC(Section* configuration):Module_base(configuration) {
		/* Register a virtual AUTOEXEC.BAT file */
		const Section_line * section=static_cast<Section_line *>(configuration);

		/* Check -securemode switch to disable mount/imgmount/boot after running autoexec.bat */
		bool secure = control->opt_securemode;

        /* The user may have given .BAT files to run on the command line */
        if (!control->auto_bat_additional.empty()) {
            std::string cmd;

            for (unsigned int i=0;i<control->auto_bat_additional.size();i++) {
				std::string batname;
				/* NTS: this code might have problems with DBCS filenames - yksoft1 */
				size_t pos = control->auto_bat_additional[i].find_last_of(CROSS_FILESPLIT);
				if(pos == std::string::npos) {  //Only a filename, mount current directory
					batname = control->auto_bat_additional[i];
					cmd += "@mount c: . -q\n";
				} else { //Parse the path of .BAT file
					std::string batpath = control->auto_bat_additional[i].substr(0,pos+1);
					batname = control->auto_bat_additional[i].substr(pos+1);
					cmd += "@mount c: " + batpath + " -q\n";
				}
				cmd += "@c:\n";
				cmd += "@cd \\\n";
                /* NTS: "CALL" does not support quoting the filename.
                 *      This will break if the batch file name has spaces in it. */
                cmd += "@CALL ";
                cmd += batname;
                cmd += "\n";
				cmd += "@mount -u c: -q\n";
            }

            autoexec_auto_bat.Install(cmd);
        }

		/* add stuff from the configfile unless -noautexec or -securemode is specified. */
		const char * extra = const_cast<char*>(section->data.c_str());
		if (extra && !secure && !control->opt_noautoexec) {
			/* detect if "echo off" is the first line */
			size_t firstline_length = strcspn(extra,"\r\n");
			bool echo_off  = !strncasecmp(extra,"echo off",8);
			if (echo_off && firstline_length == 8) extra += 8;
			else {
				echo_off = !strncasecmp(extra,"@echo off",9);
				if (echo_off && firstline_length == 9) extra += 9;
				else echo_off = false;
			}

			/* if "echo off" move it to the front of autoexec.bat */
			if (echo_off)  { 
				autoexec_echo.InstallBefore("@echo off");
				if (*extra == '\r') extra++; //It can point to \0
				if (*extra == '\n') extra++; //same
			}

			/* Install the stuff from the configfile if anything left after moving echo off */

			if (*extra) autoexec[0].Install(std::string(extra));
		}

		/* Check to see for extra command line options to be added (before the command specified on commandline) */
		/* Maximum of extra commands: 10 */
		Bitu i = 1;
		for (auto it=control->opt_c.begin();i <= 11 && it!=control->opt_c.end();it++) /* -c switches */
			autoexec[i++].Install(*it);

		/* Check for the -exit switch which causes dosbox to when the command on the commandline has finished */
		bool addexit = control->opt_exit;

#if 0/*FIXME: This is ugly. I don't care to follow through on this nonsense for now. When needed, port to new command line switching. */
		/* Check for first command being a directory or file */
		char buffer[CROSS_LEN+1];
		char orig[CROSS_LEN+1];
		char cross_filesplit[2] = {CROSS_FILESPLIT , 0};

		Bitu dummy = 1;
		bool command_found = false;
		while (control->cmdline->FindCommand(dummy++,line) && !command_found) {
			struct stat test;
			if (line.length() > CROSS_LEN) continue; 
			strcpy(buffer,line.c_str());
			if (stat(buffer,&test)) {
				if (getcwd(buffer,CROSS_LEN) == NULL) continue;
				if (strlen(buffer) + line.length() + 1 > CROSS_LEN) continue;
				strcat(buffer,cross_filesplit);
				strcat(buffer,line.c_str());
				if (stat(buffer,&test)) continue;
			}
			if (test.st_mode & S_IFDIR) { 
				autoexec[12].Install(std::string("MOUNT C \"") + buffer + "\"");
				autoexec[13].Install("C:");
				if(secure) autoexec[14].Install("z:\\config.com -securemode");
				command_found = true;
			} else {
				char* name = strrchr(buffer,CROSS_FILESPLIT);
				if (!name) { //Only a filename 
					line = buffer;
					if (getcwd(buffer,CROSS_LEN) == NULL) continue;
					if (strlen(buffer) + line.length() + 1 > CROSS_LEN) continue;
					strcat(buffer,cross_filesplit);
					strcat(buffer,line.c_str());
					if(stat(buffer,&test)) continue;
					name = strrchr(buffer,CROSS_FILESPLIT);
					if(!name) continue;
				}
				*name++ = 0;
				if (access(buffer,F_OK)) continue;
				autoexec[12].Install(std::string("MOUNT C \"") + buffer + "\"");
				autoexec[13].Install("C:");
				/* Save the non-modified filename (so boot and imgmount can use it (long filenames, case sensivitive)) */
				strcpy(orig,name);
				upcase(name);
				if(strstr(name,".BAT") != 0) {
					if(secure) autoexec[14].Install("z:\\config.com -securemode");
					/* BATch files are called else exit will not work */
					autoexec[15].Install(std::string("CALL ") + name);
					if(addexit) autoexec[16].Install("exit");
				} else if((strstr(name,".IMG") != 0) || (strstr(name,".IMA") !=0 )) {
					//No secure mode here as boot is destructive and enabling securemode disables boot
					/* Boot image files */
					autoexec[15].Install(std::string("BOOT ") + orig);
				} else if((strstr(name,".ISO") != 0) || (strstr(name,".CUE") !=0 )) {
					/* imgmount CD image files */
					/* securemode gets a different number from the previous branches! */
					autoexec[14].Install(std::string("IMGMOUNT D \"") + orig + std::string("\" -t iso"));
					//autoexec[16].Install("D:");
					if(secure) autoexec[15].Install("z:\\config.com -securemode");
					/* Makes no sense to exit here */
				} else {
					if(secure) autoexec[14].Install("z:\\config.com -securemode");
					autoexec[15].Install(name);
					if(addexit) autoexec[16].Install("exit");
				}
				command_found = true;
			}
		}

		/* Combining -securemode, noautoexec and no parameters leaves you with a lovely Z:\. */
		if ( !command_found ) { 
			if ( secure ) autoexec[12].Install("z:\\config.com -securemode");
		}
#else
		if (secure) autoexec[i++].Install("z:\\config.com -securemode");
#endif

		if (addexit) autoexec[i++].Install("exit");

		assert(i <= 17); /* FIXME: autoexec[] should not be fixed size */

		VFILE_Register("AUTOEXEC.BAT",(Bit8u *)autoexec_data,(Bit32u)strlen(autoexec_data));
	}
};

static AUTOEXEC* test = NULL;
	
static void AUTOEXEC_ShutDown(Section * sec) {
    (void)sec;//UNUSED
	if (test != NULL) {
		delete test;
		test = NULL;
	}
    if (first_shell != NULL) {
		delete first_shell;
		first_shell = 0;//Make clear that it shouldn't be used anymore
    }
    if (call_shellstop != 0) {
        CALLBACK_DeAllocate(call_shellstop);
        call_shellstop = 0;
    }
}

void AUTOEXEC_Startup(Section *sec) {
    (void)sec;//UNUSED
	if (test == NULL) {
		LOG(LOG_MISC,LOG_DEBUG)("Allocating AUTOEXEC.BAT emulation");
		test = new AUTOEXEC(control->GetSection("autoexec"));
	}
}

void AUTOEXEC_Init() {
	LOG(LOG_MISC,LOG_DEBUG)("Initializing AUTOEXEC.BAT emulation");

	AddExitFunction(AddExitFunctionFuncPair(AUTOEXEC_ShutDown));
	AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(AUTOEXEC_ShutDown));
	AddVMEventFunction(VM_EVENT_DOS_EXIT_BEGIN,AddVMEventFunctionFuncPair(AUTOEXEC_ShutDown));
	AddVMEventFunction(VM_EVENT_DOS_EXIT_REBOOT_BEGIN,AddVMEventFunctionFuncPair(AUTOEXEC_ShutDown));
	AddVMEventFunction(VM_EVENT_DOS_SURPRISE_REBOOT,AddVMEventFunctionFuncPair(AUTOEXEC_ShutDown));
}

static Bitu INT2E_Handler(void) {
	/* Save return address and current process */
	RealPt save_ret=real_readd(SegValue(ss),reg_sp);
	Bit16u save_psp=dos.psp();

	/* Set first shell as process and copy command */
	dos.psp(shell_psp);//DOS_FIRST_SHELL);
	DOS_PSP psp(shell_psp);//DOS_FIRST_SHELL);
	psp.SetCommandTail(RealMakeSeg(ds,reg_si));
	SegSet16(ss,RealSeg(psp.GetStack()));
	reg_sp=2046;

	/* Read and fix up command string */
	CommandTail tail;
	MEM_BlockRead(PhysMake(dos.psp(),CTBUF+1),&tail,CTBUF+1);
	if (tail.count<CTBUF) tail.buffer[tail.count]=0;
	else tail.buffer[CTBUF-1]=0;
	char* crlf=strpbrk(tail.buffer,"\r\n");
	if (crlf) *crlf=0;

	/* Execute command */
	if (strlen(tail.buffer)) {
		DOS_Shell temp;
		temp.ParseLine(tail.buffer);
		temp.RunInternal();
	}

	/* Restore process and "return" to caller */
	dos.psp(save_psp);
	SegSet16(cs,RealSeg(save_ret));
	reg_ip=RealOff(save_ret);
	reg_ax=0;
	return CBRET_NONE;
}

static char const * const path_string="PATH=Z:\\";
static char const * const comspec_string="COMSPEC=Z:\\COMMAND.COM";
static char const * const prompt_string="PROMPT=$P$G";
static char const * const full_name="Z:\\COMMAND.COM";
static char const * const init_line="/INIT AUTOEXEC.BAT";

extern unsigned int dosbox_shell_env_size;

/* TODO: Why is all this DOS kernel and VFILE registration here in SHELL_Init()?
 *       That's like claiming that DOS memory and device initialization happens from COMMAND.COM!
 *       We need to move the DOS kernel initialization into another function, and the VFILE
 *       registration to another function, and then message initialization to another function,
 *       and then those functions need to be called before SHELL_Init() -J.C. */
void SHELL_Init() {
	LOG(LOG_MISC,LOG_DEBUG)("Initializing DOS shell");

	/* Add messages */
	MSG_Add("SHELL_CMD_VOL_DRIVE","\n Volume in drive %c ");
	MSG_Add("SHELL_CMD_VOL_DRIVEERROR","Cannot find the drive specified\n");
	MSG_Add("SHELL_CMD_VOL_SERIAL"," Volume Serial Number is ");
	MSG_Add("SHELL_CMD_VOL_SERIAL_NOLABEL","has no label\n");
	MSG_Add("SHELL_CMD_VOL_SERIAL_LABEL","is %s\n");
	MSG_Add("SHELL_ILLEGAL_PATH","Illegal Path.\n");
	MSG_Add("SHELL_CMD_HELP","If you want a list of all supported commands type \033[33;1mhelp /all\033[0m .\nA short list of the most often used commands:\n");
	MSG_Add("SHELL_CMD_ECHO_ON","ECHO is on.\n");
	MSG_Add("SHELL_CMD_ECHO_OFF","ECHO is off.\n");
	MSG_Add("SHELL_ILLEGAL_SWITCH","Illegal switch: %s.\n");
	MSG_Add("SHELL_MISSING_PARAMETER","Required parameter missing.\n");
	MSG_Add("SHELL_CMD_CHDIR_ERROR","Unable to change to: %s.\n");
	MSG_Add("SHELL_CMD_CHDIR_HINT","Hint: To change to different drive type \033[31m%c:\033[0m\n");
	MSG_Add("SHELL_CMD_CHDIR_HINT_2","directoryname contains unquoted spaces.\nTry \033[31mcd %s\033[0m or properly quote them with quotation marks.\n");
	MSG_Add("SHELL_CMD_CHDIR_HINT_3","You are still on drive Z:, change to a mounted drive with \033[31mC:\033[0m.\n");
	MSG_Add("SHELL_CMD_DATE_HELP","Displays or changes the internal date.\n");
	MSG_Add("SHELL_CMD_DATE_ERROR","The specified date is not correct.\n");
	MSG_Add("SHELL_CMD_DATE_DAYS","3SunMonTueWedThuFriSat"); // "2SoMoDiMiDoFrSa"
	MSG_Add("SHELL_CMD_DATE_NOW","Current date: ");
	MSG_Add("SHELL_CMD_DATE_SETHLP","Type 'date MM-DD-YYYY' to change.\n");
	MSG_Add("SHELL_CMD_DATE_FORMAT","M/D/Y");
	MSG_Add("SHELL_CMD_DATE_HELP_LONG","DATE [[/T] [/H] [/S] | MM-DD-YYYY]\n"\
									"  MM-DD-YYYY: new date to set\n"\
									"  /S:         Permanently use host time and date as DOS time\n"\
                                    "  /F:         Switch back to DOSBox-X internal time (opposite of /S)\n"\
									"  /T:         Only display date\n"\
									"  /H:         Synchronize with host\n");
	MSG_Add("SHELL_CMD_TIME_HELP","Displays or changes the internal time.\n");
	MSG_Add("SHELL_CMD_TIME_ERROR","The specified time is not correct.\n");
	MSG_Add("SHELL_CMD_TIME_NOW","Current time: ");
	MSG_Add("SHELL_CMD_TIME_SETHLP","Type 'time hh:mm:ss' to change.\n");
	MSG_Add("SHELL_CMD_TIME_HELP_LONG","TIME [[/T] [/H] | hh:mm:ss]\n"\
									"  hh:mm:ss:   new time to set\n"\
									"  /T:         Display simple time\n"\
									"  /H:         Synchronize with host\n");
	MSG_Add("SHELL_CMD_MKDIR_ERROR","Unable to make: %s.\n");
	MSG_Add("SHELL_CMD_RMDIR_ERROR","Unable to remove: %s.\n");
    MSG_Add("SHELL_CMD_RENAME_ERROR","Unable to rename: %s.\n");
	MSG_Add("SHELL_CMD_ATTRIB_GET_ERROR","Unable to get attributes: %s\n");
	MSG_Add("SHELL_CMD_ATTRIB_SET_ERROR","Unable to set attributes: %s\n");
	MSG_Add("SHELL_CMD_DEL_ERROR","Unable to delete: %s.\n");
	MSG_Add("SHELL_CMD_DEL_SURE","Are you sure[Y,N]?");
	MSG_Add("SHELL_SYNTAXERROR","The syntax of the command is incorrect.\n");
	MSG_Add("SHELL_CMD_SET_NOT_SET","Environment variable %s not defined.\n");
	MSG_Add("SHELL_CMD_SET_OUT_OF_SPACE","Not enough environment space left.\n");
	MSG_Add("SHELL_CMD_IF_EXIST_MISSING_FILENAME","IF EXIST: Missing filename.\n");
	MSG_Add("SHELL_CMD_IF_ERRORLEVEL_MISSING_NUMBER","IF ERRORLEVEL: Missing number.\n");
	MSG_Add("SHELL_CMD_IF_ERRORLEVEL_INVALID_NUMBER","IF ERRORLEVEL: Invalid number.\n");
	MSG_Add("SHELL_CMD_GOTO_MISSING_LABEL","No label supplied to GOTO command.\n");
	MSG_Add("SHELL_CMD_GOTO_LABEL_NOT_FOUND","GOTO: Label %s not found.\n");
	MSG_Add("SHELL_CMD_FILE_NOT_FOUND","File %s not found.\n");
	MSG_Add("SHELL_CMD_FILE_EXISTS","File %s already exists.\n");
	MSG_Add("SHELL_CMD_DIR_INTRO"," Directory of %s\n\n");
	MSG_Add("SHELL_CMD_DIR_BYTES_USED","%5d File(s) %17s Bytes\n");
	MSG_Add("SHELL_CMD_DIR_BYTES_FREE","%5d Dir(s)  %17s Bytes free\n");
	MSG_Add("SHELL_CMD_DIR_FILES_LISTED","Total files listed:\n");
	MSG_Add("SHELL_EXECUTE_DRIVE_NOT_FOUND","Drive %c does not exist!\nYou must \033[31mmount\033[0m it first. Type \033[1;33mintro\033[0m or \033[1;33mintro mount\033[0m for more information.\n");
	MSG_Add("SHELL_EXECUTE_DRIVE_ACCESS_CDROM","Do you want to give DOSBox-X access to your real CD-ROM drive %c [Y/N]?");
	MSG_Add("SHELL_EXECUTE_DRIVE_ACCESS_FLOPPY","Do you want to give DOSBox-X access to your real floppy drive %c [Y/N]?");
	MSG_Add("SHELL_EXECUTE_DRIVE_ACCESS_FIXED","Do you really want to give DOSBox-X access to everything\non your real drive %c [Y/N]?");
	MSG_Add("SHELL_EXECUTE_DRIVE_ACCESS_WARNING_WIN","Mounting C:\\ is NOT recommended.\n");
	MSG_Add("SHELL_EXECUTE_ILLEGAL_COMMAND","Illegal command: %s.\n");
	MSG_Add("SHELL_CMD_PAUSE","Press any key to continue.\n");
	MSG_Add("SHELL_CMD_PAUSE_HELP","Waits for one keystroke to continue.\n");
	MSG_Add("SHELL_CMD_PAUSE_HELP_LONG","PAUSE\n");
	MSG_Add("SHELL_CMD_COPY_FAILURE","Copy failure : %s.\n");
	MSG_Add("SHELL_CMD_COPY_SUCCESS","   %d File(s) copied.\n");
	MSG_Add("SHELL_CMD_COPY_CONFIRM","Overwrite %s (Yes/No/All)?");
	MSG_Add("SHELL_CMD_COPY_NOSPACE","Insufficient disk space - %s\n");
	MSG_Add("SHELL_CMD_COPY_ERROR","Error in copying file %s\n");
	MSG_Add("SHELL_CMD_SUBST_DRIVE_LIST","To list all currently mounted drives, use MOUNT command without a parameter.\n");
	MSG_Add("SHELL_CMD_SUBST_NO_REMOVE","Unable to remove, drive not in use.\n");
	MSG_Add("SHELL_CMD_SUBST_IN_USE","Target drive is already in use.\n");
	MSG_Add("SHELL_CMD_SUBST_NOT_LOCAL","It is only possible to use SUBST on local drives.\n");
	MSG_Add("SHELL_CMD_SUBST_INVALID_PATH","The specified drive or path is invalid.\n");
	MSG_Add("SHELL_CMD_SUBST_FAILURE","SUBST: There is an error in your command line.\n");

    std::string mapper_keybind = mapper_event_keybind_string("host");
    if (mapper_keybind.empty()) mapper_keybind = "unbound";

    /* Capitalize the binding */
    if (mapper_keybind.size() > 0)
        mapper_keybind[0] = toupper(mapper_keybind[0]);

    /* Punctuation is important too. */
    mapper_keybind += ".";

    /* NTS: MSG_Add() takes the string as const char * but it does make a copy of the string when entering into the message map,
     *      so there is no problem here of causing use-after-free crashes when we exit. */
    std::string host_key_help; // SHELL_STARTUP_BEGIN2

    if (machine == MCH_PC98) {
// "\x86\x46 To activate the keymapper \033[31mhost+M\033[37m. Host key is F12.                 \x86\x46\n"
        host_key_help =
            std::string("\x86\x46 To activate the keymapper \033[31mhost+M\033[37m. Host key is ") +
            (mapper_keybind + "                                     ").substr(0,20) +
            std::string(" \x86\x46\n");
    }
    else {
// "\xBA To activate the keymapper \033[31mhost+M\033[37m. Host key is F12.                 \xBA\n"
        host_key_help =
            std::string("\033[44;1m\xBA To activate the keymapper \033[31mhost+M\033[37m. Host key is ") +
            (mapper_keybind + "                                     ").substr(0,20) +
            std::string(" \xBA\033[0m\n");
    }

    if (machine == MCH_PC98) {
        MSG_Add("SHELL_STARTUP_BEGIN",
                "\x86\x52\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x56\n"
                "\x86\x46 \033[32mWelcome to DOSBox-X %-8s (%-4s) %-25s\033[37m      \x86\x46\n"
                "\x86\x46                                                                    \x86\x46\n"
                "\x86\x46 For a short introduction for new users type: \033[33mINTRO\033[37m                 \x86\x46\n"
                "\x86\x46 For supported shell commands type: \033[33mHELP\033[37m                            \x86\x46\n"
                "\x86\x46                                                                    \x86\x46\n"
                "\x86\x46 To adjust the emulated CPU speed, use \033[31mhost -\033[37m and \033[31mhost +\033[37m.           \x86\x46\n");
        MSG_Replace("SHELL_STARTUP_BEGIN2",
                host_key_help.c_str());
        MSG_Add("SHELL_STARTUP_BEGIN3",
                "\x86\x46 For more information read the online guide in the \033[36mDOSBox-X Wiki\033[37m.   \x86\x46\n"
                "\x86\x46                                                                    \x86\x46\n"
               );
        MSG_Add("SHELL_STARTUP_PC98","\x86\x46 DOSBox-X is now running in NEC PC-98 emulation mode.               \x86\x46\n"
                "\x86\x46 \033[31mPC-98 emulation is INCOMPLETE and CURRENTLY IN DEVELOPMENT.\033[37m        \x86\x46\n");
        MSG_Add("SHELL_STARTUP_DEBUG",
#if defined(MACOSX)
                "\x86\x46 Debugger is available, use \033[31mAlt-F12\033[37m to enter.                       \x86\x46\n"
#else
                "\x86\x46 Debugger is available, use \033[31mAlt-Pause\033[37m to enter.                     \x86\x46\n"
#endif
                "\x86\x46                                                                    \x86\x46\n"
               );
        MSG_Add("SHELL_STARTUP_END",
                "\x86\x46 \033[32mDOSBox-X project \033[33mhttp://dosbox-x.software\033[32m      PentiumPro support \033[37m \x86\x46\n"
                "\x86\x46 \033[32mDerived from DOSBox \033[33mhttp://www.dosbox.com\033[37m                          \x86\x46\n"
                "\x86\x5A\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44"
                "\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x44\x86\x5E\033[0m\n"
                "\033[1m\033[32mHAVE FUN!\033[0m\n"
               );
    }
    else {
        MSG_Add("SHELL_STARTUP_BEGIN",
                "\033[44;1m\xC9\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
                "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
                "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBB\033[0m\n"
                "\033[44;1m\xBA \033[32mWelcome to DOSBox-X %-8s (%-4s) %-25s\033[37m      \xBA\033[0m\n"
                "\033[44;1m\xBA                                                                    \xBA\033[0m\n"
                "\033[44;1m\xBA For a short introduction for new users type: \033[33mINTRO\033[37m                 \xBA\033[0m\n"
                "\033[44;1m\xBA For supported shell commands type: \033[33mHELP\033[37m                            \xBA\033[0m\n"
                "\033[44;1m\xBA                                                                    \xBA\033[0m\n"
                "\033[44;1m\xBA To adjust the emulated CPU speed, use \033[31mhost -\033[37m and \033[31mhost +\033[37m.           \xBA\033[0m\n"
			   );
        MSG_Replace("SHELL_STARTUP_BEGIN2",
                host_key_help.c_str());
        MSG_Add("SHELL_STARTUP_BEGIN3",
                "\033[44;1m\xBA For more information read the online guide in the \033[36mDOSBox-X Wiki\033[37m.   \xBA\033[0m\n"
                "\033[44;1m\xBA                                                                    \xBA\033[0m\n"
               );
        if (!mono_cga) {
            MSG_Add("SHELL_STARTUP_CGA","\033[44;1m\xBA DOSBox-X supports Composite CGA mode.                              \xBA\033[0m\n"
                    "\033[44;1m\xBA Use \033[31mF12\033[37m to set composite output ON, OFF, or AUTO (default).        \xBA\033[0m\n"
                    "\033[44;1m\xBA \033[31m(Alt-)F11\033[37m changes hue; \033[31mctrl-alt-F11\033[37m selects early/late CGA model.  \xBA\033[0m\n"
                    "\033[44;1m\xBA                                                                    \xBA\033[0m\n"
                   );
        } else {
            MSG_Add("SHELL_STARTUP_CGA","\033[44;1m\xBA Use \033[31mF11\033[37m to cycle through green, amber, and white monochrome color, \xBA\033[0m\n"
                    "\033[44;1m\xBA and \033[31mAlt-F11\033[37m to change contrast/brightness settings.                \xBA\033[0m\n"
                   );
        }
        MSG_Add("SHELL_STARTUP_PC98","\xBA DOSBox-X is now running in NEC PC-98 emulation mode.               \xBA\n"
                "\xBA \033[31mPC-98 emulation is INCOMPLETE and CURRENTLY IN DEVELOPMENT.\033[37m        \xBA\n");
        MSG_Add("SHELL_STARTUP_HERC","\033[44;1m\xBA Use F11 to cycle through white, amber, and green monochrome color. \xBA\033[0m\n"
                "\033[44;1m\xBA Use alt-F11 to toggle horizontal blending (only in graphics mode). \xBA\033[0m\n"
                "\033[44;1m\xBA                                                                    \xBA\033[0m\n"
               );
        MSG_Add("SHELL_STARTUP_DEBUG",
#if defined(MACOSX)
                "\033[44;1m\xBA Debugger is available, use \033[31mAlt-F12\033[37m to enter.                       \xBA\033[0m\n"
#else
                "\033[44;1m\xBA Debugger is available, use \033[31mAlt-Pause\033[37m to enter.                     \xBA\033[0m\n"
#endif
                "\033[44;1m\xBA                                                                    \xBA\033[0m\n"
               );
        MSG_Add("SHELL_STARTUP_END",
                "\033[44;1m\xBA \033[32mDOSBox-X project \033[33mhttp://dosbox-x.software\033[32m      PentiumPro support \033[37m \xBA\033[0m\n"
                "\033[44;1m\xBA \033[32mDerived from DOSBox \033[33mhttp://www.dosbox.com\033[37m                          \xBA\033[0m\n"
                "\033[44;1m\xC8\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
                "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD"
                "\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xCD\xBC\033[0m\n"
                "\033[1m\033[32mHAVE FUN!\033[0m\n"
               );
    }

	MSG_Add("SHELL_CMD_BREAK_HELP","Sets or clears extended CTRL+C checking.\n");
	MSG_Add("SHELL_CMD_BREAK_HELP_LONG","BREAK [ON | OFF]\n\nType BREAK without a parameter to display the current BREAK setting.\n");
	MSG_Add("SHELL_CMD_CHDIR_HELP","Displays or changes the current directory.\n");
	MSG_Add("SHELL_CMD_CHDIR_HELP_LONG","CHDIR [drive:][path]\n"
	        "CHDIR [..]\n"
	        "CD [drive:][path]\n"
	        "CD [..]\n\n"
	        "  ..   Specifies that you want to change to the parent directory.\n\n"
	        "Type CD drive: to display the current directory in the specified drive.\n"
	        "Type CD without parameters to display the current drive and directory.\n");
	MSG_Add("SHELL_CMD_CLS_HELP","Clears screen.\n");
	MSG_Add("SHELL_CMD_CLS_HELP_LONG","CLS\n");
	MSG_Add("SHELL_CMD_DIR_HELP","Displays a list of files and subdirectories in a directory.\n");
	MSG_Add("SHELL_CMD_DIR_HELP_LONG","DIR [drive:][path][filename] [/[W|B]] [/S] [/P] [/A[D|S|H|R|A]] [/O[N|E|S|D]]\n\n"
		   "   [drive:][path][filename]\n"
		   "       Specifies drive, directory, and/or files to list.\n\n"
		   "   /W\tUses wide list format.\n"
		   "   /B\tUses bare format (no heading information or summary).\n"
		   "   /S\tDisplays files in specified directory and all subdirectories.\n"
		   "   /P\tPauses after each screenful of information.\n"
		   "   /A\tDisplays all files and directories.\n"
		   "   /AD\tDisplays all directories.\n"
		   "   /AS\tDisplays files with system attributes.\n"
		   "   /AH\tDisplays files with hidden attributes.\n"
		   "   /AR\tDisplays files with read-only attributes.\n"
		   "   /AA\tDisplays files with archive attributes.\n"
		   "   /ON\tList files sorted by name (alphabetic).\n"
		   "   /OE\tList files sorted by extension (alphabetic).\n"
		   "   /OS\tList files sorted by size (smallest first).\n"
		   "   /OD\tList files sorted by date (oldest first).\n\n"
		   "   The \"-\" sign can be used in /A[D|S|H|R|A] and /O[N|E|S|D] commands,\n"
		   "   meaning \"not\". For example, /A-D displays all files (not directories),\n"
		   "   and /O-S lists files reversely sorted by size (biggest first).\n"
		   );
	MSG_Add("SHELL_CMD_ECHO_HELP","Displays messages, or turns command-echoing on or off.\n");
	MSG_Add("SHELL_CMD_ECHO_HELP_LONG","  ECHO [ON | OFF]\n  ECHO [message]\n\nType ECHO without parameters to display the current echo setting.\n");
	MSG_Add("SHELL_CMD_EXIT_HELP","Exits from the command shell.\n");
	MSG_Add("SHELL_CMD_EXIT_HELP_LONG","EXIT\n");
	MSG_Add("SHELL_CMD_HELP_HELP","Shows command help.\n");
	MSG_Add("SHELL_CMD_HELP_HELP_LONG","HELP\n");
	MSG_Add("SHELL_CMD_MKDIR_HELP","Creates a directory.\n");
	MSG_Add("SHELL_CMD_MKDIR_HELP_LONG","MKDIR [drive:][path]\n"
	        "MD [drive:][path]\n");
	MSG_Add("SHELL_CMD_RMDIR_HELP","Removes a directory.\n");
	MSG_Add("SHELL_CMD_RMDIR_HELP_LONG","RMDIR [drive:][path]\n"
	        "RD [drive:][path]\n");
	MSG_Add("SHELL_CMD_SET_HELP","Displays or changes environment variables.\n");
	MSG_Add("SHELL_CMD_SET_HELP_LONG","SET [variable=[string]]\n\n"
		   "   variable\tSpecifies the environment-variable name.\n"
		   "   string\tSpecifies a series of characters to assign to the variable.\n\n"
		   "* If no string is specified, the variable is removed from the environment.\n\n"
		   "Type SET without parameters to display the current environment variables.\n");
	MSG_Add("SHELL_CMD_IF_HELP","Performs conditional processing in batch programs.\n");
	MSG_Add("SHELL_CMD_IF_HELP_LONG","IF [NOT] ERRORLEVEL number command\n"
		   "IF [NOT] string1==string2 command\n"
		   "IF [NOT] EXIST filename command\n\n"
		   "  NOT               Specifies that DOS should carry out\n"
		   "                    the command only if the condition is false.\n\n"
		   "  ERRORLEVEL number Specifies a true condition if the last program run\n"
		   "                    returned an exit code equal to or greater than the number\n"
		   "                    specified.\n\n"
		   "  string1==string2  Specifies a true condition if the specified text strings\n"
		   "                    match.\n\n"
		   "  EXIST filename    Specifies a true condition if the specified filename\n"
		   "                    exists.\n\n"
		   "  command           Specifies the command to carry out if the condition is\n"
		   "                    met.  Command can be followed by ELSE command which\n"
		   "                    will execute the command after the ELSE keyword if the\n"
		   "                    specified condition is FALSE\n");
	MSG_Add("SHELL_CMD_GOTO_HELP","Jumps to a labeled line in a batch program.\n");
	MSG_Add("SHELL_CMD_GOTO_HELP_LONG","GOTO label\n\n"
		   "   label   Specifies a text string used in the batch program as a label.\n\n"
		   "You type a label on a line by itself, beginning with a colon.\n");
	MSG_Add("SHELL_CMD_SHIFT_HELP","Changes the position of replaceable parameters in a batch file.\n");
	MSG_Add("SHELL_CMD_SHIFT_HELP_LONG","SHIFT\n");
	MSG_Add("SHELL_CMD_FOR_HELP","Runs a specified command for each file in a set of files.\n");
	MSG_Add("SHELL_CMD_FOR_HELP_LONG","FOR %%variable IN (set) DO command [command-parameters]\n\n  %%variable  Specifies a replaceable parameter.\n  (set)      Specifies a set of one or more files. Wildcards may be used.\n  command    Specifies the command to carry out for each file.\n  command-parameters\n             Specifies parameters or switches for the specified command.\n\nTo use the command in a batch program, specify %%%%variable instead of %%variable.\n");
	MSG_Add("SHELL_CMD_LFNFOR_HELP","Enables or disables long filenames when processing FOR wildcards.\n");
	MSG_Add("SHELL_CMD_LFNFOR_HELP_LONG","LFNFOR [ON | OFF]\n\nType LFNFOR without a parameter to display the current LFNFOR setting.\n\nThis command is only useful if LFN support is currently enabled.\n");
	MSG_Add("SHELL_CMD_TYPE_HELP","Displays the contents of a text-file.\n");
	MSG_Add("SHELL_CMD_TYPE_HELP_LONG","TYPE [drive:][path][filename]\n");
	MSG_Add("SHELL_CMD_REM_HELP","Adds comments in a batch file.\n");
	MSG_Add("SHELL_CMD_REM_HELP_LONG","REM [comment]\n");
	MSG_Add("SHELL_CMD_RENAME_HELP","Renames one or more files.\n");
	MSG_Add("SHELL_CMD_RENAME_HELP_LONG","RENAME [drive:][path]filename1 filename2.\n"
	        "REN [drive:][path]filename1 filename2.\n\n"
	        "Note that you can not specify a new drive or path for your destination file.\n");
	MSG_Add("SHELL_CMD_DELETE_HELP","Removes one or more files.\n");
	MSG_Add("SHELL_CMD_DELETE_HELP_LONG","DEL [/P] [/Q] names\n"
		   "ERASE [/P] [/Q] names\n\n"
		   "  names\t\tSpecifies a list of one or more files or directories.\n"
		   "\t\tWildcards may be used to delete multiple files. If a\n"
		   "\t\tdirectory is specified, all files within the directory\n"
		   "\t\twill be deleted.\n"
		   "  /P\t\tPrompts for confirmation before deleting one or more files.\n"
		   "  /Q\t\tQuiet mode, do not ask if ok to delete on global wildcard\n");
	MSG_Add("SHELL_CMD_COPY_HELP","Copies one or more files.\n");
	MSG_Add("SHELL_CMD_COPY_HELP_LONG","COPY [/Y | /-Y] source [+source [+ ...]] [destination]\n\n"
		   "  source\tSpecifies the file or files to be copied.\n"
		   "  destination\tSpecifies the directory and/or filename for the new file(s).\n"
		   "  /Y\t\tSuppresses prompting to confirm you want to overwrite an\n\t\texisting destination file.\n"
		   "  /-Y\t\tCauses prompting to confirm you want to overwrite an\n\t\texisting destination file.\n");
	MSG_Add("SHELL_CMD_CALL_HELP","Starts a batch file from within another batch file.\n");
	MSG_Add("SHELL_CMD_CALL_HELP_LONG","CALL [drive:][path]filename [batch-parameters]\n\n"
		   "batch-parameters   Specifies any command-line information required by\n"
		   "                   the batch program.\n");
	MSG_Add("SHELL_CMD_SUBST_HELP","Assigns an internal directory to a drive.\n");
	MSG_Add("SHELL_CMD_SUBST_HELP_LONG","SUBST [drive1: [drive2:]path]\nSUBST drive1: /D\n");
	MSG_Add("SHELL_CMD_LOADHIGH_HELP","Loads a program into upper memory (requires xms=true,umb=true).\n");
	MSG_Add("SHELL_CMD_LOADHIGH_HELP_LONG","LH\t\t[drive1:][path]filename [parameters]\nLOADHIGH\t[drive1:][path]filename [parameters]\n");
	MSG_Add("SHELL_CMD_CHOICE_HELP","Waits for a keypress and sets ERRORLEVEL.\n");
	MSG_Add("SHELL_CMD_CHOICE_HELP_LONG","CHOICE [/C:choices] [/N] [/S] text\n"
	        "  /C[:]choices  -  Specifies allowable keys.  Default is: yn.\n"
	        "  /N  -  Do not display the choices at end of prompt.\n"
	        "  /S  -  Enables case-sensitive choices to be selected.\n"
	        "  text  -  The text to display as a prompt.\n");
	MSG_Add("SHELL_CMD_ATTRIB_HELP","Displays or changes file attributes.\n");
	MSG_Add("SHELL_CMD_ATTRIB_HELP_LONG","ATTRIB [+R | -R] [+A | -A] [+S | -S] [+H | -H] [drive:][path][filename] [/S]\n\n"
			"  +	Sets an attribute.\n"
			"  -	Clears an attribute.\n"
			"  R	Read-only file attribute.\n"
			"  A	Archive file attribute.\n"
			"  S	System file attribute.\n"
			"  H	Hidden file attribute.\n"
			"  [drive:][path][filename] Specifies file(s) for ATTRIB to process.\n"
			"  /S Processes files in all directories in the specified path.\n");
	MSG_Add("SHELL_CMD_PATH_HELP","Displays or sets a search path for executable files.\n");
	MSG_Add("SHELL_CMD_PATH_HELP_LONG","PATH [[drive:]path[;...][;%PATH%]\n"
		   "PATH ;\n\n"
		   "Type PATH ; to clear all search path settings.\n"
		   "Type PATH without parameters to display the current path.\n");
	MSG_Add("SHELL_CMD_VERIFY_HELP","Controls whether to verify that your files are written correctly to a disk.\n");
	MSG_Add("SHELL_CMD_VERIFY_HELP_LONG","VERIFY [ON | OFF]\n\nType VERIFY without a parameter to display the current VERIFY setting.\n");
	MSG_Add("SHELL_CMD_VER_HELP","Displays or sets DOSBox-X's reported DOS version.\n");
	MSG_Add("SHELL_CMD_VER_HELP_LONG","VER\n" 
		   "VER SET [major minor] or VER SET [major.minor]\n\n" 
		   "  [major minor] or [major.minor]  Set the reported DOS version.\n"
		   "  e.g. \"VER SET 5 0\" or \"VER SET 7.1\" for DOS 5.0 or 7.1 resp.\n\n" 
		   "Type VER without parameters to display the current DOS version.\n");
	MSG_Add("SHELL_CMD_VER_VER","DOSBox-X version %s (%s). Reported DOS version %d.%02d.\n");
	MSG_Add("SHELL_CMD_ADDKEY_HELP","Generates artificial keypresses.\n");
	MSG_Add("SHELL_CMD_ADDKEY_HELP_LONG","ADDKEY [key]\n");
	MSG_Add("SHELL_CMD_VOL_HELP","Displays the disk volume label and serial number, if they exist.\n");
	MSG_Add("SHELL_CMD_VOL_HELP_LONG","VOL [drive]\n");
	MSG_Add("SHELL_CMD_PROMPT_HELP","Changes the command prompt.\n");
	MSG_Add("SHELL_CMD_PROMPT_HELP_LONG","PROMPT [text]\n"
		   "  text    Specifies a new command prompt.\n\n"
		   "Prompt can be made up of normal characters and the following special codes:\n"
		   "  $A   & (Ampersand)\n"
		   "  $B   | (pipe)\n"
		   "  $C   ( (Left parenthesis)\n"
		   "  $D   Current date\n"
		   "  $E   Escape code (ASCII code 27)\n"
		   "  $F   ) (Right parenthesis)\n"
		   "  $G   > (greater-than sign)\n"
		   "  $H   Backspace (erases previous character)\n"
		   "  $L   < (less-than sign)\n"
		   "  $N   Current drive\n"
		   "  $P   Current drive and path\n"
		   "  $Q   = (equal sign)\n"
		   "  $S     (space)\n"
		   "  $T   Current time\n"
		   "  $V   DOS version number\n"
		   "  $_   Carriage return and linefeed\n"
		   "  $$   $ (dollar sign)\n");
	MSG_Add("SHELL_CMD_LABEL_HELP","Creates or changes the volume label of a disk.\n");
	MSG_Add("SHELL_CMD_LABEL_HELP_LONG","LABEL [volume]\n\n\tvolume\t\tSpecifies the drive letter.\n");
    MSG_Add("SHELL_CMD_ALIAS_HELP", "Defines or displays aliases.\n");
    MSG_Add("SHELL_CMD_ALIAS_HELP_LONG", "ALIAS [name[=value] ... ]\n\nType ALIAS without parameters to display the list of aliases in the form:\n`ALIAS NAME = VALUE'\n");
    MSG_Add("SHELL_CMD_CTTY_HELP","Changes the terminal device used to control the system.\n");
	MSG_Add("SHELL_CMD_CTTY_HELP_LONG","CTTY device\n  device\tThe terminal device to use, such as CON.\n");
	MSG_Add("SHELL_CMD_MORE_HELP","Displays output one screen at a time.\n");
	MSG_Add("SHELL_CMD_MORE_HELP_LONG","MORE [drive:][path][filename]\nMORE < [drive:][path]filename\ncommand-name | MORE [drive:][path][filename]\n");
	MSG_Add("SHELL_CMD_TRUENAME_HELP","Finds the fully-expanded name for a file.\n");
	MSG_Add("SHELL_CMD_TRUENAME_HELP_LONG","TRUENAME file\n");
	MSG_Add("SHELL_CMD_COMMAND_HELP","Starts the DOSBox-X command shell.\n\nThe following options are accepted:\n\n  /C\tExecutes the specified command and returns.\n  /K\tExecutes the specified command and continues running.\n  /INIT\tInitializes the command shell.\n");

	/* Regular startup */
	call_shellstop=CALLBACK_Allocate();
	/* Setup the startup CS:IP to kill the last running machine when exitted */
	RealPt newcsip=CALLBACK_RealPointer(call_shellstop);
	SegSet16(cs,RealSeg(newcsip));
	reg_ip=RealOff(newcsip);

	CALLBACK_Setup(call_shellstop,shellstop_handler,CB_IRET,"shell stop");
	PROGRAMS_MakeFile("COMMAND.COM",SHELL_ProgramStart);

    /* NTS: Some DOS programs behave badly if run from a command interpreter
     *      who's PSP segment is too low in memory and does not appear in
     *      the MCB chain (SimCity 2000). So allocate shell memory normally
     *      as any DOS application would do.
     *
     *      That includes allocating COMMAND.COM stack NORMALLY (not up in
     *      the UMB as DOSBox SVN would do) */

	/* Now call up the shell for the first time */
	Bit16u psp_seg;//=DOS_FIRST_SHELL;
	Bit16u env_seg;//=DOS_FIRST_SHELL+19; //DOS_GetMemory(1+(4096/16))+1;
	Bit16u stack_seg;//=DOS_GetMemory(2048/16,"COMMAND.COM stack");
    Bit16u tmp,total_sz;

    // decide shell env size
    if (dosbox_shell_env_size == 0)
        dosbox_shell_env_size = (0x158u - (0x118u + 19u)) << 4u; /* equivalent to mainline DOSBox */
    else
        dosbox_shell_env_size = (dosbox_shell_env_size+15u)&(~15u); /* round up to paragraph */

    LOG_MSG("COMMAND.COM env size:             %u bytes",dosbox_shell_env_size);

    // According to some sources, 0x0008 is a special PSP segment value used by DOS before the first
    // program is used. We need the current PSP segment to be nonzero so that DOS_AllocateMemory()
    // can properly allocate memory.
    dos.psp(8);

    // COMMAND.COM environment block
    tmp = dosbox_shell_env_size>>4;
	if (!DOS_AllocateMemory(&env_seg,&tmp)) E_Exit("COMMAND.COM failed to allocate environment block segment");
    LOG_MSG("COMMAND.COM environment block:    0x%04x sz=0x%04x",env_seg,tmp);

    // COMMAND.COM main binary (including PSP and stack)
    tmp = 0x1A + (2048/16);
    total_sz = tmp;
	if (!DOS_AllocateMemory(&psp_seg,&tmp)) E_Exit("COMMAND.COM failed to allocate main body + PSP segment");
    LOG_MSG("COMMAND.COM main body (PSP):      0x%04x sz=0x%04x",psp_seg,tmp);

    // now COMMAND.COM has a main body and PSP segment, reflect it
    dos.psp(psp_seg);
    shell_psp = psp_seg;

    {
        DOS_MCB mcb((Bit16u)(env_seg-1));
        mcb.SetPSPSeg(psp_seg);
        mcb.SetFileName("COMMAND");
    }

    {
        DOS_MCB mcb((Bit16u)(psp_seg-1));
        mcb.SetPSPSeg(psp_seg);
        mcb.SetFileName("COMMAND");
    }

    // set the stack at 0x1A
    stack_seg = psp_seg + 0x1A;
    LOG_MSG("COMMAND.COM stack:                0x%04x",stack_seg);

    // set the stack pointer
	SegSet16(ss,stack_seg);
	reg_sp=2046;

	/* Set up int 24 and psp (Telarium games) */
	real_writeb(psp_seg+16+1,0,0xea);		/* far jmp */
	real_writed(psp_seg+16+1,1,real_readd(0,0x24*4));
	real_writed(0,0x24*4,((Bit32u)psp_seg<<16) | ((16+1)<<4));

	/* Set up int 23 to "int 20" in the psp. Fixes what.exe */
	real_writed(0,0x23*4,((Bit32u)psp_seg<<16));

	/* Set up int 2e handler */
    if (call_int2e == 0)
        call_int2e = CALLBACK_Allocate();

    //	RealPt addr_int2e=RealMake(psp_seg+16+1,8);
    // NTS: It's apparently common practice to enumerate MCBs by reading the segment value of INT 2Eh and then
    //      scanning forward from there. The assumption seems to be that COMMAND.COM writes INT 2Eh there using
    //      it's PSP segment and an offset like that of a COM executable even though COMMAND.COM is often an EXE file.
	RealPt addr_int2e=RealMake(psp_seg,8+((16+1)*16));

    CALLBACK_Setup(call_int2e,&INT2E_Handler,CB_IRET_STI,Real2Phys(addr_int2e),"Shell Int 2e");
	RealSetVec(0x2e,addr_int2e);

	/* Setup environment */
	PhysPt env_write=PhysMake(env_seg,0);
	MEM_BlockWrite(env_write,path_string,(Bitu)(strlen(path_string)+1));
	env_write += (PhysPt)(strlen(path_string)+1);
	MEM_BlockWrite(env_write,comspec_string,(Bitu)(strlen(comspec_string)+1));
	env_write += (PhysPt)(strlen(comspec_string)+1);
	MEM_BlockWrite(env_write,prompt_string,(Bitu)(strlen(prompt_string)+1));
	env_write +=(PhysPt)(strlen(prompt_string)+1);
	mem_writeb(env_write++,0);
	mem_writew(env_write,1);
	env_write+=2;
	MEM_BlockWrite(env_write,full_name,(Bitu)(strlen(full_name)+1));

//    extern bool Mouse_Vertical;
	extern bool Mouse_Drv;
	Mouse_Drv = true;

	VFILE_RegisterBuiltinFileBlob(bfb_DEBUG_EXE);
	VFILE_RegisterBuiltinFileBlob(bfb_MOVE_EXE);
	VFILE_RegisterBuiltinFileBlob(bfb_FIND_EXE);
	VFILE_RegisterBuiltinFileBlob(bfb_LASTDRIV_COM);
	VFILE_RegisterBuiltinFileBlob(bfb_FCBS_COM);
	VFILE_RegisterBuiltinFileBlob(bfb_XCOPY_EXE);
	VFILE_RegisterBuiltinFileBlob(bfb_APPEND_EXE);
	VFILE_RegisterBuiltinFileBlob(bfb_DEVICE_COM);
	VFILE_RegisterBuiltinFileBlob(bfb_BUFFERS_COM);

    /* These are IBM PC/XT/AT ONLY. They will not work in PC-98 mode. */
    if (!IS_PC98_ARCH) {
        VFILE_RegisterBuiltinFileBlob(bfb_HEXMEM16_EXE);
        VFILE_RegisterBuiltinFileBlob(bfb_HEXMEM32_EXE);
        VFILE_RegisterBuiltinFileBlob(bfb_DOSIDLE_EXE);
        VFILE_RegisterBuiltinFileBlob(bfb_CWSDPMI_EXE);
        VFILE_RegisterBuiltinFileBlob(bfb_DOS32A_EXE);
        VFILE_RegisterBuiltinFileBlob(bfb_DOS4GW_EXE);
        VFILE_RegisterBuiltinFileBlob(bfb_EDIT_COM);
        VFILE_RegisterBuiltinFileBlob(bfb_TREE_EXE);

        if (IS_VGA_ARCH)
            VFILE_RegisterBuiltinFileBlob(bfb_25_COM);
        else if (IS_EGA_ARCH)
            VFILE_RegisterBuiltinFileBlob(bfb_25_COM_ega);
        else
            VFILE_RegisterBuiltinFileBlob(bfb_25_COM_other);
    }

    /* MEM.COM is not compatible with PC-98 and/or 8086 emulation */
    if (!IS_PC98_ARCH && CPU_ArchitectureType >= CPU_ARCHTYPE_80186)
        VFILE_RegisterBuiltinFileBlob(bfb_MEM_COM);

    /* DSXMENU.EXE */
    if (IS_PC98_ARCH)
        VFILE_RegisterBuiltinFileBlob(bfb_DSXMENU_EXE_PC98);
    else
        VFILE_RegisterBuiltinFileBlob(bfb_DSXMENU_EXE_PC);

    /* don't register 28.com unless EGA/VGA */
    if (IS_VGA_ARCH)
        VFILE_RegisterBuiltinFileBlob(bfb_28_COM);
    else if (IS_EGA_ARCH)
        VFILE_RegisterBuiltinFileBlob(bfb_28_COM_ega);

    /* don't register 50 unless VGA */
    if (IS_VGA_ARCH) VFILE_RegisterBuiltinFileBlob(bfb_50_COM);

	DOS_PSP psp(psp_seg);
	psp.MakeNew(0);
	dos.psp(psp_seg);
   
	/* The start of the filetable in the psp must look like this:
	 * 01 01 01 00 02
	 * In order to achieve this: First open 2 files. Close the first and
	 * duplicate the second (so the entries get 01) */
	Bit16u dummy=0;
	DOS_OpenFile("CON",OPEN_READWRITE,&dummy);	/* STDIN  */
	DOS_OpenFile("CON",OPEN_READWRITE,&dummy);	/* STDOUT */
	DOS_CloseFile(0);							/* Close STDIN */
	DOS_ForceDuplicateEntry(1,0);				/* "new" STDIN */
	DOS_ForceDuplicateEntry(1,2);				/* STDERR */
	DOS_OpenFile("CON",OPEN_READWRITE,&dummy);	/* STDAUX */
	if (!DOS_OpenFile("PRN",OPEN_READWRITE,&dummy)) DOS_OpenFile("CON",OPEN_READWRITE,&dummy);	/* STDPRN */

    psp.SetSize(psp_seg + total_sz);
    psp.SetStack(((unsigned int)stack_seg << 16u) + (unsigned int)reg_sp);
	psp.SetParent(psp_seg);
	/* Set the environment */
	psp.SetEnvironment(env_seg);
	/* Set the command line for the shell start up */
	CommandTail tail;
	tail.count=(Bit8u)strlen(init_line);
	memset(&tail.buffer, 0, CTBUF);
	strncpy(tail.buffer,init_line,CTBUF);
	MEM_BlockWrite(PhysMake(psp_seg,CTBUF+1),&tail,CTBUF+1);
	
	/* Setup internal DOS Variables */
	dos.dta(RealMake(psp_seg,CTBUF+1));
	dos.psp(psp_seg);

    /* settings */
    {
        const Section_prop * section=static_cast<Section_prop *>(control->GetSection("dos"));
        enable_config_as_shell_commands = section->Get_bool("shell configuration as commands");
    }
}

/* Pfff... starting and running the shell from a configuration section INIT
 * What the hell were you guys thinking? --J.C. */
void SHELL_Run() {
	dos_shell_running_program = false;
#if defined(WIN32) && !defined(C_SDL2)
	int Reflect_Menu(void);
	Reflect_Menu();
#endif

	LOG(LOG_MISC,LOG_DEBUG)("Running DOS shell now");

	if (first_shell != NULL) E_Exit("Attempt to start shell when shell already running");
	SHELL_ProgramStart_First_shell(&first_shell);

	try {
		first_shell->Run();
		delete first_shell;
		first_shell = 0;//Make clear that it shouldn't be used anymore
		dos_shell_running_program = false;
#if defined(WIN32) && !defined(C_SDL2)
		int Reflect_Menu(void);
		Reflect_Menu();
#endif
	}
	catch (...) {
		delete first_shell;
		first_shell = 0;//Make clear that it shouldn't be used anymore
		dos_shell_running_program = false;
#if defined(WIN32) && !defined(C_SDL2)
		int Reflect_Menu(void);
		Reflect_Menu();
#endif
		throw;
	}
}
