// -*- C++ -*-
//*************************************************************************
//
// Copyright 2000-2010 by Wilson Snyder.  This program is free software;
// you can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License Version 2.0.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//*************************************************************************
/// \file
/// \brief Verilog::Preproc: Internal implementation of default preprocessor
///
/// Authors: Wilson Snyder
///
/// Code available from: http://www.veripool.org/verilog-perl
///
//*************************************************************************

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <stack>
#include <vector>
#include <map>
#include <list>
#include <cassert>
#include <cerrno>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#if defined(_WIN32) && !defined(__MINGW32__) && !defined(__CYGWIN__)
# include <io.h>
#else
# include <unistd.h>
#endif

#include "VPreProc.h"
#include "VPreLex.h"

//#undef yyFlexLexer
//#define yyFlexLexer xxFlexLexer
//#include <FlexLexer.h>

//*************************************************************************

class VPreDefRef {
    // One for each pending define substitution
    string	m_name;		// Define last name being defined
    string	m_params;	// Define parameter list for next expansion
    string	m_nextarg;	// String being built for next argument
    int		m_parenLevel;	// Parenthesis counting inside def args (for PARENT not child)

    vector<string> m_args;	// List of define arguments
public:
    string name() const { return m_name; }
    string params() const { return m_params; }
    string nextarg() const { return m_nextarg; }
    void nextarg(const string& value) { m_nextarg = value; }
    int parenLevel() const { return m_parenLevel; }
    void parenLevel(int value) { m_parenLevel = value; }
    vector<string>& args() { return m_args; }
    VPreDefRef(const string& name, const string& params)
	: m_name(name), m_params(params), m_parenLevel(0) {}
    ~VPreDefRef() {}
};

//*************************************************************************
/// Data for parsing on/off

class VPreIfEntry {
    // One for each pending ifdef/ifndef
    bool	m_on;		// Current parse for this ifdef level is "on"
    bool	m_everOn;	// Some if term in elsif tree has been on
public:
    bool on() const { return m_on; }
    bool everOn() const { return m_everOn; }
    VPreIfEntry(bool on, bool everOn)
	: m_on(on), m_everOn(everOn || on) {}  // Note everOn includes new state
    ~VPreIfEntry() {}
};

//*************************************************************************
/// Data for a preprocessor instantiation.

struct VPreProcImp : public VPreProcOpaque {
    typedef list<string> StrList;

    VPreProc*	m_preprocp;	///< Object we're holding data for
    VFileLine*	m_filelinep;	///< Last token's starting point
    int		m_debug;	///< Debugging level
    VPreLex* m_lexp;	///< Current lexer state (NULL = closed)
    stack<VPreLex*> m_includeStack;	///< Stack of includers above current m_lexp

    enum ProcState { ps_TOP,
		     ps_DEFNAME_UNDEF, ps_DEFNAME_DEFINE,
		     ps_DEFNAME_IFDEF, ps_DEFNAME_IFNDEF, ps_DEFNAME_ELSIF,
		     ps_DEFFORM, ps_DEFVALUE, ps_DEFPAREN, ps_DEFARG,
		     ps_INCNAME, ps_ERRORNAME };
    const char* procStateName(ProcState s) {
	static const char* states[]
	    = {"ps_TOP",
	       "ps_DEFNAME_UNDEF", "ps_DEFNAME_DEFINE",
	       "ps_DEFNAME_IFDEF", "ps_DEFNAME_IFNDEF", "ps_DEFNAME_ELSIF",
	       "ps_DEFFORM", "ps_DEFVALUE", "ps_DEFPAREN", "ps_DEFARG",
	       "ps_INCNAME", "ps_ERRORNAME"};
	return states[s];
    };

    stack<ProcState>	m_states; ///< Current state of parser
    int		m_off;		///< If non-zero, ifdef level is turned off, don't dump text
    string	m_lastSym;	///< Last symbol name found.
    string	m_formals;	///< Last formals found

    // For getRawToken/ `line insertion
    string	m_lineCmt;	///< Line comment(s) to be returned
    bool	m_lineCmtNl;	///< Newline needed before inserting lineCmt
    int		m_lineAdd;	///< Empty lines to return to maintain line count
    bool	m_rawAtBol;	///< Last rawToken left us at beginning of line

    // For defines
    stack<VPreDefRef> m_defRefs; // Pending definine substitution
    stack<VPreIfEntry> m_ifdefStack;	///< Stack of true/false emitting evaluations
    unsigned	m_defDepth;	///< How many `defines deep

    // For getline()
    string	m_lineChars;	///< Characters left for next line

    VPreProcImp(VFileLine* filelinep) {
	m_filelinep = filelinep;
	m_debug = 0;
	m_lexp = NULL;	 // Closed.
	m_states.push(ps_TOP);
	m_off = 0;
	m_lineChars = "";
	m_lastSym = "";
	m_lineAdd = 0;
	m_lineCmtNl = false;
	m_rawAtBol = true;
	m_defDepth = 0;
    }
    ~VPreProcImp() {
	if (m_lexp) { delete m_lexp; m_lexp = NULL; }
    }
    const char* tokenName(int tok);
    int getRawToken();
    int getToken();
    void debugToken(int tok, const char* cmtp);
    void parseTop();
    void parseUndef();
    string getparseline(bool stop_at_eol, size_t approx_chunk);
    bool isEof() const { return (m_lexp==NULL); }
    bool readWholefile(const string& filename, StrList& outl);
    void openFile(string filename, VFileLine* filelinep);
    void insertUnreadback(const string& text) { m_lineCmt += text; }
    void insertUnreadbackAtBol(const string& text);
private:
    void error(string msg) { m_filelinep->error(msg); }
    void fatal(string msg) { m_filelinep->fatal(msg); }
    int debug() const { return m_debug; }
    void eof();
    string defineSubst(VPreDefRef* refp);
    void addLineComment(int enter_exit_level);
    string trimWhitespace(const string& strg, bool trailing);
    void unputString(const string& strg);

    void parsingOn() { m_off--; assert(m_off>=0); if (!m_off) addLineComment(0); }
    void parsingOff() { m_off++; }

    void statePush(ProcState state) {
	m_states.push(state);
    }
    void statePop() {
	m_states.pop();
	if (m_states.empty()) {
	    error("InternalError: Pop of parser state with nothing on stack");
	    m_states.push(ps_TOP);
	}
    }
    void stateChange(ProcState state) {
	statePop(); statePush(state);
    }

};

//*************************************************************************
// Creation

VPreProc::VPreProc(VFileLine* filelinep) {
    VPreProcImp* idatap = new VPreProcImp(filelinep);
    m_opaquep = idatap;
    idatap->m_preprocp = this;
}

VPreProc::~VPreProc() {
    if (m_opaquep) { delete m_opaquep; m_opaquep = NULL; }
}

//*************************************************************************
// VPreProc Methods.  Just call the implementation functions.

void VPreProc::comment(string cmt) { }
void VPreProc::openFile(string filename, VFileLine* filelinep) {
    VPreProcImp* idatap = static_cast<VPreProcImp*>(m_opaquep);
    idatap->openFile (filename,filelinep);
}
string VPreProc::getline() {
    VPreProcImp* idatap = static_cast<VPreProcImp*>(m_opaquep);
    return idatap->getparseline(true,0);
}
string VPreProc::getall(size_t approx_chunk) {
    VPreProcImp* idatap = static_cast<VPreProcImp*>(m_opaquep);
    return idatap->getparseline(false,approx_chunk);
}
void VPreProc::debug(int level) {
    VPreProcImp* idatap = static_cast<VPreProcImp*>(m_opaquep);
    idatap->m_debug = level;
    // To see "accepting rule" debug, Makefile.PL must be changed to enable flex debug
}
bool VPreProc::isEof() {
    VPreProcImp* idatap = static_cast<VPreProcImp*>(m_opaquep);
    return idatap->isEof();
}
VFileLine* VPreProc::filelinep() {
    VPreProcImp* idatap = static_cast<VPreProcImp*>(m_opaquep);
    return idatap->m_filelinep;
}
void VPreProc::insertUnreadback(string text) {
    VPreProcImp* idatap = static_cast<VPreProcImp*>(m_opaquep);
    return idatap->insertUnreadback(text);
}

//**********************************************************************
// Parser Utilities

const char* VPreProcImp::tokenName(int tok) {
    switch (tok) {
    case VP_EOF		: return("EOF");
    case VP_INCLUDE	: return("INCLUDE");
    case VP_IFDEF	: return("IFDEF");
    case VP_IFNDEF	: return("IFNDEF");
    case VP_ENDIF	: return("ENDIF");
    case VP_UNDEF	: return("UNDEF");
    case VP_DEFINE	: return("DEFINE");
    case VP_ELSE	: return("ELSE");
    case VP_ELSIF	: return("ELSIF");
    case VP_LINE	: return("LINE");
    case VP_SYMBOL	: return("SYMBOL");
    case VP_STRING	: return("STRING");
    case VP_DEFFORM	: return("DEFFORM");
    case VP_DEFVALUE	: return("DEFVALUE");
    case VP_COMMENT	: return("COMMENT");
    case VP_TEXT	: return("TEXT");
    case VP_WHITE	: return("WHITE");
    case VP_DEFREF	: return("DEFREF");
    case VP_DEFARG	: return("DEFARG");
    case VP_ERROR	: return("ERROR");
    case VP_UNDEFINEALL	: return("UNDEFINEALL");
    default: return("?");
    }
}

void VPreProcImp::unputString(const string& strg) {
    // Note: The preliminary call in ::openFile bypasses this function
    // We used to just m_lexp->unputString(strg.c_str());
    // However this can lead to "flex scanner push-back overflow"
    // so instead we scan from a temporary buffer, then on EOF return.
    // This is also faster than the old scheme, amazingly.
    if (1) {
	if (m_lexp->m_bufferStack.empty() || m_lexp->m_bufferStack.top()!=m_lexp->currentBuffer()) {
	    fatalSrc("bufferStack missing current buffer; will return incorrectly");
	    // Hard to debug lost text as won't know till much later
	}
    }
    m_lexp->scanBytes(strg.c_str(), strg.length());
}

string VPreProcImp::trimWhitespace(const string& strg, bool trailing) {
    // Remove leading whitespace
    string out = strg;
    string::size_type leadspace = 0;
    while (out.length() > leadspace
	   && isspace(out[leadspace])) leadspace++;
    if (leadspace) out.erase(0,leadspace);
    // Remove trailing whitespace
    if (trailing) {
	string::size_type trailspace = 0;
	while (out.length() > trailspace
	       && isspace(out[out.length()-1-trailspace])) trailspace++;
	if (trailspace) out.erase(out.length()-trailspace,trailspace);
    }
    return out;
}

string VPreProcImp::defineSubst(VPreDefRef* refp) {
    // Substitute out defines in a define reference.
    // (We also need to call here on non-param defines to handle `")
    // We could push the define text back into the lexer, but that's slow
    // and would make recursive definitions and parameter handling nasty.
    //
    // Note we parse the definition parameters and value here.  If a
    // parametrized define is used many, many times, we could cache the
    // parsed result.
    if (debug()) {
	cout<<"defineSubstIn  `"<<refp->name()<<" "<<refp->params()<<endl;
	for (unsigned i=0; i<refp->args().size(); i++) {
	    cout<<"defineArg["<<i<<"] = '"<<refp->args()[i]<<"'"<<endl;
	}
    }
    // Grab value
    string value = m_preprocp->defValue(refp->name());
    if (debug()) cout<<"defineValue    '"<<value<<"'"<<endl;

    map<string,string> argValueByName;
    {   // Parse argument list into map
	unsigned numArgs=0;
	string argName;
	int paren = 1;  // (), {} and [] can use same counter, as must be matched pair per spec
	string token;
	bool quote = false;
	bool haveDefault = false;
	// Note there's a leading ( and trailing ), so parens==1 is the base parsing level
	const char* cp=refp->params().c_str();
	if (*cp == '(') cp++;
	for (; *cp; cp++) {
	    //if (debug()) cout <<"   Parse  Paren="<<paren<<"  Arg="<<numArgs<<"  token='"<<token<<"'  Parse="<<cp<<endl;
	    if (!quote && paren==1) {
		if (*cp==')' || *cp==',') {
		    string value;
		    if (haveDefault) { value=token; } else { argName=token; }
		    argName = trimWhitespace(argName,true);
		    if (debug()) cout<<"    Got Arg="<<numArgs<<"  argName='"<<argName<<"'  default='"<<value<<"'"<<endl;
		    // Parse it
		    if (argName!="") {
			if (refp->args().size() > numArgs) {
			    // A call `def( a ) must be equivelent to `def(a ), so trimWhitespace
			    // Note other sims don't trim trailing whitespace, so we don't either.
			    string arg = trimWhitespace(refp->args()[numArgs], false);
			    if (arg != "") value = arg;
			} else if (!haveDefault) {
			    error("Define missing argument '"+argName+"' for: "+refp->name()+"\n");
			    return " `"+refp->name()+" ";
			}
			numArgs++;
		    }
		    argValueByName[argName] = value;
		    // Prepare for next
		    argName = "";
		    token = "";
		    haveDefault = false;
		    continue;
		}
		else if (*cp=='=') {
		    haveDefault = true;
		    argName = token;
		    token = "";
		    continue;
		}
	    }
	    if (cp[0]=='\\' && cp[1]) {
		token += cp[0]; // \{any} Put out literal next character
		token += cp[1];
		cp++;
		continue;
	    }
	    if (!quote) {
		if (*cp=='(' || *cp=='{' || *cp=='[') paren++;
		else if (*cp==')' || *cp=='}' || *cp==']') paren--;
	    }
	    if (*cp=='"') quote=!quote;
	    if (*cp) token += *cp;
	}
	if (refp->args().size() > numArgs
	    // `define X() is ok to call with nothing
	    && !(refp->args().size()==1 && numArgs==0 && trimWhitespace(refp->args()[0],false)=="")) {
	    error("Define passed too many arguments: "+refp->name()+"\n");
	    return " `"+refp->name()+" ";
	}
    }

    string out = "";
    {   // Parse substitution define using arguments
	string argName;
	string prev;
	bool quote = false;
	// Note we go through the loop once more at the NULL end-of-string
	for (const char* cp=value.c_str(); (*cp) || argName!=""; cp=(*cp?cp+1:cp)) {
	    //cout << "CH "<<*cp<<"  an "<<argName<<"\n";
	    if (!quote) {
		if ( isalpha(*cp) || *cp=='_'
		     || *cp=='$' // Won't replace system functions, since no $ in argValueByName
		     || (argName!="" && (isdigit(*cp) || *cp=='$'))) {
		    argName += *cp;
		    continue;
		}
	    }
	    if (argName != "") {
		// Found a possible variable substitution
		map<string,string>::iterator iter = argValueByName.find(argName);
		if (iter != argValueByName.end()) {
		    // Substitute
		    string subst = iter->second;
		    out += subst;
		} else {
		    out += argName;
		}
		argName = "";
	    }
	    if (!quote) {
		// Check for `` only after we've detected end-of-argname
		if (cp[0]=='`' && cp[1]=='`') {
		    //out += "";   // `` means to suppress the ``
		    cp++;
		    continue;
		}
		else if (cp[0]=='`' && cp[1]=='"') {
		    out += '"';   // `" means to put out a " without enabling quote mode (sort of)
		    cp++;
		    continue;
		}
		else if (cp[0]=='`' && cp[1]=='\\') {
		    out += '\\';   // `\ means to put out a backslash
		    cp++;
		    continue;
		}
	    }
	    if (cp[0]=='\\' && cp[1]) {
		out += cp[0]; // \{any} Put out literal next character
		out += cp[1];
		cp++;
		continue;
	    }
	    if (*cp=='"') quote=!quote;
	    if (*cp) out += *cp;
	}
    }

    if (debug()) cout<<"defineSubstOut '"<<out<<"'"<<endl;
    return out;
}

//**********************************************************************
// Parser routines

bool VPreProcImp::readWholefile(const string& filename, StrList& outl) {
    int fd = open (filename.c_str(), O_RDONLY);
    if (!fd) return false;

    // If change this code, run a test with the below size set very small
//#define INFILTER_IPC_BUFSIZ 16
#define INFILTER_IPC_BUFSIZ 64*1024
    char buf[INFILTER_IPC_BUFSIZ];
    bool eof = false;
    while (!eof) {
	ssize_t todo = INFILTER_IPC_BUFSIZ;
	ssize_t got = read (fd, buf, todo);
	if (got>0) {
	    outl.push_back(string(buf, got));
	}
	else if (errno == EINTR || errno == EAGAIN
#ifdef EWOULDBLOCK
		 || errno == EWOULDBLOCK
#endif
	    ) {
	} else { eof = true; break; }
    }

    close(fd);
    return true;
}

void VPreProcImp::openFile(string filename, VFileLine* filelinep) {
    // Open a new file, possibly overriding the current one which is active.
    if (filelinep) {
	m_filelinep = filelinep;
    }

    // Read a list<string> with the whole file.
    StrList wholefile;
    bool ok = readWholefile(filename, wholefile/*ref*/);
    if (!ok) {
	error("File not found: "+filename+"\n");
	return;
    }

    if (m_lexp) {
	// We allow the same include file twice, because occasionally it pops
	// up, with guards preventing a real recursion.
	if (m_includeStack.size()>VPreProc::INCLUDE_DEPTH_MAX) {
	    error("Recursive inclusion of file: "+filename);
	    return;
	}
	// There's already a file active.  Push it to work on the new one.
	m_includeStack.push(m_lexp);
	addLineComment(0);
    }

    m_lexp = new VPreLex ();
    m_lexp->m_keepComments = m_preprocp->keepComments();
    m_lexp->m_keepWhitespace = m_preprocp->keepWhitespace();
    m_lexp->m_pedantic = m_preprocp->pedantic();
    m_lexp->m_curFilelinep = m_preprocp->filelinep()->create(filename, 1);
    m_filelinep = m_lexp->m_curFilelinep;  // Remember token start location
    addLineComment(1); // Enter

    // Filter all DOS CR's en-mass.  This avoids bugs with lexing CRs in the wrong places.
    // This will also strip them from strings, but strings aren't supposed to be multi-line without a "\"
    for (StrList::iterator it=wholefile.begin(); it!=wholefile.end(); ++it) {
	// We don't end-loop at \0 as we allow and strip mid-string '\0's (for now).
	bool strip = false;
	const char* sp = it->data();
	const char* ep = sp + it->length();
	// Only process if needed, as saves extra string allocations
	for (const char* cp=sp; cp<ep; cp++) {
	    if (*cp == '\r' || *cp == '\0') {
		strip = true; break;
	    }
	}
	if (strip) {
	    string out;  out.reserve(it->length());
	    for (const char* cp=sp; cp<ep; cp++) {
		if (!(*cp == '\r' || *cp == '\0')) {
		    out += *cp;
		}
	    }
	    *it = out;
	}

	// Push the data to an internal buffer.
	m_lexp->scanBytesBack(*it);
	// Reclaim memory; the push saved the string contents for us
	*it = "";
    }
}

void VPreProcImp::insertUnreadbackAtBol(const string& text) {
    // Insert insuring we're at the beginning of line, for `line
    // We don't always add a leading newline, as it may result in extra unreadback(newlines).
    if (m_lineCmt == "") { m_lineCmtNl = true; }
    else if (m_lineCmt[m_lineCmt.length()-1]!='\n') {
	insertUnreadback("\n");
    }
    insertUnreadback(text);
}

void VPreProcImp::addLineComment(int enter_exit_level) {
    if (m_preprocp->lineDirectives()) {
	insertUnreadbackAtBol(m_lexp->curFilelinep()->lineDirectiveStrg(enter_exit_level));
    }
}

void VPreProcImp::eof() {
    // Perhaps we're completing unputString
    if (m_lexp->m_bufferStack.size()>1) {
	if (debug()) cout<<m_filelinep<<"EOS\n";
	// Switch to file or next unputString, but not a eof so don't delete lexer
	yy_delete_buffer(m_lexp->currentBuffer());
	m_lexp->m_bufferStack.pop();  // Must work as size>1
	yy_switch_to_buffer(m_lexp->m_bufferStack.top());
    } else {
	// Remove current lexer
	if (debug()) cout<<m_filelinep<<"EOF!\n";
	addLineComment(2);	// Exit
	// Destructor will call yy_delete_buffer
	delete m_lexp;  m_lexp=NULL;
	// Perhaps there's a parent file including us?
	if (!m_includeStack.empty()) {
	    // Back to parent.
	    m_lexp = m_includeStack.top(); m_includeStack.pop();
	    addLineComment(0);
	    if (m_lexp->m_bufferStack.empty()) fatalSrc("No include buffer to return to");
	    yy_switch_to_buffer(m_lexp->m_bufferStack.top());  // newest buffer in older lexer
	}
    }
}

int VPreProcImp::getRawToken() {
    // Get a token from the file, whatever it may be.
    while (1) {
      next_tok:
	if (m_lineAdd) {
	    m_lineAdd--;
	    m_rawAtBol = true;
	    yyourtext("\n",1);
	    if (debug()) debugToken(VP_WHITE, "LNA");
	    return (VP_WHITE);
	}
	if (m_lineCmt!="") {
	    // We have some `line directive or other processed data to return to the user.
	    static string rtncmt;  // Keep the c string till next call
	    rtncmt = m_lineCmt;
	    if (m_lineCmtNl) {
		if (!m_rawAtBol) rtncmt = "\n"+rtncmt;
		m_lineCmtNl = false;
	    }
	    yyourtext(rtncmt.c_str(), rtncmt.length());
	    m_lineCmt = "";
	    if (yyourleng()) m_rawAtBol = (yyourtext()[yyourleng()-1]=='\n');
	    if (m_states.top()==ps_DEFVALUE) {
		VPreLex::s_currentLexp->appendDefValue(yyourtext(),yyourleng());
		goto next_tok;
	    } else {
		if (debug()) debugToken(VP_TEXT, "LCM");
		return (VP_TEXT);
	    }
	}
	if (isEof()) return (VP_EOF);

	// Snarf next token from the file
	m_filelinep = m_lexp->curFilelinep();  // Remember token start location
	VPreLex::s_currentLexp = m_lexp;   // Tell parser where to get/put data
	int tok = yylex();

	if (debug()) debugToken(tok, "RAW");

	// On EOF, try to pop to upper level includes, as needed.
	if (tok==VP_EOF) {
	    eof();
	    goto next_tok;  // Parse parent, or find the EOF.
	}

	if (yyourleng()) m_rawAtBol = (yyourtext()[yyourleng()-1]=='\n');
	return tok;
    }
}

void VPreProcImp::debugToken(int tok, const char* cmtp) {
    if (debug()) {
	string buf = string (yyourtext(), yyourleng());
	string::size_type pos;
	while ((pos=buf.find("\n")) != string::npos) { buf.replace(pos, 1, "\\n"); }
	while ((pos=buf.find("\r")) != string::npos) { buf.replace(pos, 1, "\\r"); }
	fprintf (stderr, "%d: %s %s %s(%d) dr%d:  <%d>%-10s: %s\n",
		 m_filelinep->lineno(), cmtp, m_off?"of":"on",
		 procStateName(m_states.top()), (int)m_states.size(), (int)m_defRefs.size(),
		 m_lexp->currentStartState(), tokenName(tok), buf.c_str());
    }
}

// Sorry, we're not using bison/yacc. It doesn't handle returning white space
// in the middle of parsing other tokens.

int VPreProcImp::getToken() {
    // Return the next user-visible token in the input stream.
    // Includes and such are handled here, and are never seen by the caller.
    while (1) {
      next_tok:
	if (isEof()) return VP_EOF;
	int tok = getRawToken();
	// Always emit white space and comments between tokens.
	if (tok==VP_WHITE) return (tok);
	if (tok==VP_COMMENT) {
	    if (!m_off && m_lexp->m_keepComments) {
		if (m_lexp->m_keepComments == KEEPCMT_SUB
		    || m_lexp->m_keepComments == KEEPCMT_EXP) {
		    string rtn; rtn.assign(yyourtext(),yyourleng());
		    m_preprocp->comment(rtn);
		} else {
		    return (tok);
		}
	    }
	    // We're off or processed the comment specially.  If there are newlines
	    // in it, we also return the newlines as TEXT so that the linenumber
	    // count is maintained for downstream tools
	    for (size_t len=0; len<(size_t)yyourleng(); len++) { if (yyourtext()[len]=='\n') m_lineAdd++; }
	    goto next_tok;
	}
	if (tok==VP_LINE) {
	    addLineComment(m_lexp->m_enterExit);
	    goto next_tok;
	}
	// Deal with some special parser states
	ProcState state = m_states.top();
	switch (state) {
	case ps_TOP: {
	    break;
	}
	case ps_DEFNAME_UNDEF:	// FALLTHRU
	case ps_DEFNAME_DEFINE:	// FALLTHRU
	case ps_DEFNAME_IFDEF:	// FALLTHRU
	case ps_DEFNAME_IFNDEF:	// FALLTHRU
	case ps_DEFNAME_ELSIF: {
	    if (tok==VP_SYMBOL) {
		m_lastSym.assign(yyourtext(),yyourleng());
		if (state==ps_DEFNAME_IFDEF
		    || state==ps_DEFNAME_IFNDEF) {
		    bool enable = m_preprocp->defExists(m_lastSym);
		    if (debug()) cout<<"Ifdef "<<m_lastSym<<(enable?" ON":" OFF")<<endl;
		    if (state==ps_DEFNAME_IFNDEF) enable = !enable;
		    m_ifdefStack.push(VPreIfEntry(enable,false));
		    if (!enable) parsingOff();
		    statePop();
		    goto next_tok;
		}
		else if (state==ps_DEFNAME_ELSIF) {
		    if (m_ifdefStack.empty()) {
			error("`elsif with no matching `if\n");
		    } else {
			// Handle `else portion
			VPreIfEntry lastIf = m_ifdefStack.top(); m_ifdefStack.pop();
			if (!lastIf.on()) parsingOn();
			// Handle `if portion
			bool enable = !lastIf.everOn() && m_preprocp->defExists(m_lastSym);
			if (debug()) cout<<"Elsif "<<m_lastSym<<(enable?" ON":" OFF")<<endl;
			m_ifdefStack.push(VPreIfEntry(enable, lastIf.everOn()));
			if (!enable) parsingOff();
		    }
		    statePop();
		    goto next_tok;
		}
		else if (state==ps_DEFNAME_UNDEF) {
		    if (!m_off) {
			if (debug()) cout<<"Undef "<<m_lastSym<<endl;
			m_preprocp->undef(m_lastSym);
		    }
		    statePop();
		    goto next_tok;
		}
		else if (state==ps_DEFNAME_DEFINE) {
		    // m_lastSym already set.
		    stateChange(ps_DEFFORM);
		    m_lexp->pushStateDefForm();
		    goto next_tok;
		}
		else fatalSrc("Bad case\n");
		goto next_tok;
	    }
	    else if (tok==VP_TEXT) {
		// IE, something like comment between define and symbol
		if (!m_off) return tok;
		else goto next_tok;
	    }
	    else if (tok==VP_DEFREF) {
		// IE, `ifdef `MACRO(x): Substitue and come back here when state pops.
		break;
	    }
	    else {
		error((string)"Expecting define name. Found: "+tokenName(tok)+"\n");
		goto next_tok;
	    }
	}
	case ps_DEFFORM: {
	    if (tok==VP_DEFFORM) {
		m_formals = m_lexp->m_defValue;
		if (debug()) cout<<"DefFormals='"<<m_formals<<"'\n";
		stateChange(ps_DEFVALUE);
		m_lexp->pushStateDefValue();
		goto next_tok;
	    } else if (tok==VP_TEXT) {
		// IE, something like comment in formals
		if (!m_off) return tok;
		else goto next_tok;
	    } else {
		error((string)"Expecting define formal arguments. Found: "+tokenName(tok)+"\n");
		goto next_tok;
	    }
	}
	case ps_DEFVALUE: {
	    static string newlines;
	    newlines = "\n";  // Always start with trailing return
	    if (tok == VP_DEFVALUE) {
		if (debug()) cout<<"DefValue='"<<m_lexp->m_defValue<<"'  formals='"<<m_formals<<"'\n";
		// Add any formals
		string formals = m_formals;
		string value = m_lexp->m_defValue;
		// Remove returns
		// Not removing returns in values has two problems,
		// 1. we need to correct line numbers with `line after each substitution
		// 2. Substituting in " .... " with embedded returns requires \ escape.
		//    This is very difficult in the presence of `".
		for (size_t i=0; i<formals.length(); i++) {
		    if (formals[i] == '\n') {
			formals[i] = ' ';
			newlines += "\n";
		    }
		}
		for (size_t i=0; i<value.length(); i++) {
		    if (value[i] == '\n') {
			value[i] = ' ';
			newlines += "\n";
		    }
		}
		if (!m_off) {
		    // Remove leading and trailing whitespace
		    value = trimWhitespace(value, true);
		    // Define it
		    if (debug()) cout<<"Define "<<m_lastSym<<" "<<formals<<" = '"<<value<<"'"<<endl;
		    m_preprocp->define(m_lastSym, value, formals);
		}
	    } else {
		string msg = string("Bad define text, unexpected ")+tokenName(tok)+"\n";
		fatalSrc(msg);
	    }
	    statePop();
	    // DEFVALUE is terminated by a return, but lex can't return both tokens.
	    // Thus, we emit a return here.
	    yyourtext(newlines.c_str(), newlines.length());
	    return(VP_WHITE);
	}
	case ps_DEFPAREN: {
	    if (tok==VP_TEXT && yyourleng()==1 && yyourtext()[0]=='(') {
		stateChange(ps_DEFARG);
		goto next_tok;
	    } else {
		if (m_defRefs.empty()) error("InternalError: Shouldn't be in DEFPAREN w/o active defref");
		VPreDefRef* refp = &(m_defRefs.top());
		error((string)"Expecting ( to begin argument list for define reference `"+refp->name()+"\n");
		statePop();
		goto next_tok;
	    }
	}
	case ps_DEFARG: {
	    if (m_defRefs.empty()) error("InternalError: Shouldn't be in DEFARG w/o active defref");
	    VPreDefRef* refp = &(m_defRefs.top());
	    refp->nextarg(refp->nextarg()+m_lexp->m_defValue); m_lexp->m_defValue="";
	    if (debug()) cout<<"defarg++ "<<refp->nextarg()<<endl;
	    if (tok==VP_DEFARG && yyourleng()==1 && yyourtext()[0]==',') {
		refp->args().push_back(refp->nextarg());
		stateChange(ps_DEFARG);
		m_lexp->pushStateDefArg(1);
		refp->nextarg("");
		goto next_tok;
	    } else if (tok==VP_DEFARG && yyourleng()==1 && yyourtext()[0]==')') {
		refp->args().push_back(refp->nextarg());
		string out = defineSubst(refp);
		// Substitute in and prepare for next action
		// Similar code in non-parenthesized define (Search for END_OF_DEFARG)
		m_defRefs.pop();
		out = m_preprocp->defSubstitute(out);
		if (m_defRefs.empty()) {
		    unputString(out);
		    statePop();
		    m_lexp->m_parenLevel = 0;
		}
		else {  // Finished a defref inside a upper defref
		    // Can't subst now, or
		    // `define a(ign) x,y
		    // foo(`a(ign),`b)  would break because a contains comma
		    refp = &(m_defRefs.top());  // We popped, so new top
		    refp->nextarg(refp->nextarg()+m_lexp->m_defValue+out); m_lexp->m_defValue="";
		    m_lexp->m_parenLevel = refp->parenLevel();
		    statePop();  // Will go to ps_DEFARG, as we're under another define
		}
		goto next_tok;
	    } else if (tok==VP_DEFREF) {
		// Expand it, then state will come back here
		// Value of building argument is data before the lower defref
		// we'll append it when we push the argument.
		break;
	    } else if (tok==VP_SYMBOL || tok==VP_STRING || VP_TEXT || VP_WHITE) {
		string rtn; rtn.assign(yyourtext(),yyourleng());
		refp->nextarg(refp->nextarg()+rtn);
		goto next_tok;
	    } else {
		error((string)"Expecting ) or , to end argument list for define reference. Found: "+tokenName(tok));
		statePop();
		goto next_tok;
	    }
	}
	case ps_INCNAME: {
	    if (tok==VP_STRING) {
		statePop();
		m_lastSym.assign(yyourtext(),yyourleng());
		if (debug()) cout<<"Include "<<m_lastSym<<endl;
		// Drop leading and trailing quotes.
		m_lastSym.erase(0,1);
		m_lastSym.erase(m_lastSym.length()-1,1);
		m_preprocp->include(m_lastSym);
		goto next_tok;
	    }
	    else if (tok==VP_TEXT && yyourleng()==1 && yyourtext()[0]=='<') {
		// include <filename>
		stateChange(ps_INCNAME);  // Still
		m_lexp->pushStateIncFilename();
		goto next_tok;
	    }
	    else if (tok==VP_DEFREF) {
		// Expand it, then state will come back here
		break;
	    }
	    else {
		statePop();
		error((string)"Expecting include filename. Found: "+tokenName(tok)+"\n");
		goto next_tok;
	    }
	}
	case ps_ERRORNAME: {
	    if (tok==VP_STRING) {
		if (!m_off) {
		    m_lastSym.assign(yyourtext(),yyourleng());
		    error(m_lastSym);
		}
		statePop();
		goto next_tok;
	    }
	    else {
		error((string)"Expecting `error string. Found: "+tokenName(tok)+"\n");
		statePop();
		goto next_tok;
	    }
	}
	default: fatalSrc("Bad case\n");
	}
	// Default is to do top level expansion of some tokens
	switch (tok) {
	case VP_INCLUDE:
	    if (!m_off) {
		statePush(ps_INCNAME);
	    }
	    goto next_tok;
	case VP_UNDEF:
	    statePush(ps_DEFNAME_UNDEF);
	    goto next_tok;
	case VP_DEFINE:
	    statePush(ps_DEFNAME_DEFINE);
	    goto next_tok;
	case VP_IFDEF:
	    statePush(ps_DEFNAME_IFDEF);
	    goto next_tok;
	case VP_IFNDEF:
	    statePush(ps_DEFNAME_IFNDEF);
	    goto next_tok;
	case VP_ELSIF:
	    statePush(ps_DEFNAME_ELSIF);
	    goto next_tok;
	case VP_ELSE:
	    if (m_ifdefStack.empty()) {
		error("`else with no matching `if\n");
	    } else {
		VPreIfEntry lastIf = m_ifdefStack.top(); m_ifdefStack.pop();
		bool enable = !lastIf.everOn();
		if (debug()) cout<<"Else "<<(enable?" ON":" OFF")<<endl;
		m_ifdefStack.push(VPreIfEntry(enable, lastIf.everOn()));
		if (!lastIf.on()) parsingOn();
		if (!enable) parsingOff();
	    }
	    goto next_tok;
	case VP_ENDIF:
	    if (debug()) cout<<"Endif "<<endl;
	    if (m_ifdefStack.empty()) {
		error("`endif with no matching `if\n");
	    } else {
		VPreIfEntry lastIf = m_ifdefStack.top(); m_ifdefStack.pop();
		if (!lastIf.on()) parsingOn();
		// parsingOn() really only enables parsing if
		// all ifdef's above this want it on
	    }
	    goto next_tok;

	case VP_DEFREF: {
	    if (!m_off) {
		string name; name.append(yyourtext()+1,yyourleng()-1);
		if (debug()) cout<<"DefRef "<<name<<endl;
		if (m_defDepth++ > VPreProc::DEFINE_RECURSION_LEVEL_MAX) {
		    error("Recursive `define substitution: `"+name);
		    goto next_tok;
		}
		//
		string params = m_preprocp->defParams(name);
		if (params=="") {   // Not found, return original string as-is
		    m_defDepth = 0;
		    if (debug()) cout<<"Defref `"<<name<<" => not_defined"<<endl;
		    return (VP_TEXT);
		}
		else if (params=="0") {  // Found, as simple substitution
		    string out = m_preprocp->defValue(name);
		    if (debug()) cout<<"Defref `"<<name<<" => '"<<out<<"'"<<endl;
		    out = m_preprocp->defSubstitute(out);
		    // Similar code in parenthesized define (Search for END_OF_DEFARG)
		    if (m_defRefs.empty()) {
			// Just output the substitution
			unputString(out);
		    } else {  // Inside another define.
			// Can't subst now, or
			// `define a x,y
			// foo(`a,`b)  would break because a contains comma
			VPreDefRef* refp = &(m_defRefs.top());
			refp->nextarg(refp->nextarg()+m_lexp->m_defValue+out); m_lexp->m_defValue="";
		    }
		    goto next_tok;
		}
		else {  // Found, with parameters
		    if (debug()) cout<<"Defref `"<<name<<" => parametrized"<<endl;
		    // The CURRENT macro needs the paren saved, it's not a property of the child macro
		    if (!m_defRefs.empty()) m_defRefs.top().parenLevel(m_lexp->m_parenLevel);
		    m_defRefs.push(VPreDefRef(name, params));
		    statePush(ps_DEFPAREN);
		    m_lexp->pushStateDefArg(0);
		    goto next_tok;
		}
		fatalSrc("Bad case\n");
	    }
	    else goto next_tok;
	}
	case VP_ERROR: {
	    statePush(ps_ERRORNAME);
	    goto next_tok;
	}
	case VP_EOF:
	    if (!m_ifdefStack.empty()) {
		error("`ifdef not terminated at EOF\n");
	    }
	    return tok;
	case VP_UNDEFINEALL:
	    if (!m_off) {
		if (debug()) cout<<"Undefineall "<<endl;
		m_preprocp->undefineall();
	    }
	    goto next_tok;
	case VP_SYMBOL:
	case VP_STRING:
	case VP_TEXT: {
	    m_defDepth = 0;
	    if (!m_off) return tok;
	    else goto next_tok;
	}
	case VP_WHITE:		// Handled at top of loop
	case VP_COMMENT:	// Handled at top of loop
	case VP_DEFFORM:	// Handled by state=ps_DEFFORM;
	case VP_DEFVALUE:	// Handled by state=ps_DEFVALUE;
	default:
	    fatalSrc("Internal error: Unexpected token.\n");
	    break;
	}
	return tok;
    }
}

string VPreProcImp::getparseline(bool stop_at_eol, size_t approx_chunk) {
    // Get a single line from the parse stream.  Buffer unreturned text until the newline.
    if (isEof()) return "";
    while (1) {
	const char* rtnp = NULL;
	bool gotEof = false;
	while ((stop_at_eol
		? (NULL==(rtnp=strchr(m_lineChars.c_str(),'\n')))
		: (approx_chunk==0 || (m_lineChars.length() < approx_chunk)))
	       && !gotEof) {
	    int tok = getToken();
	    if (debug()) {
		string buf = string (yyourtext(), yyourleng());
		string::size_type pos;
		while ((pos=buf.find("\n")) != string::npos) { buf.replace(pos, 1, "\\n"); }
		while ((pos=buf.find("\r")) != string::npos) { buf.replace(pos, 1, "\\r"); }
		fprintf (stderr,"%d: GETFETC:  %-10s: %s\n",
			 m_filelinep->lineno(), tokenName(tok), buf.c_str());
	    }
	    if (tok==VP_EOF) {
		// Add a final newline, if the user forgot the final \n.
		// Note tok==VP_EOF isn't always seen by us, as isEof() may be set earlier
		if (m_lineChars != "" && m_lineChars[m_lineChars.length()-1] != '\n') {
		    m_lineChars.append("\n");
		}
		gotEof = true;
	    }
	    else {
		m_lineChars.append(yyourtext(),0,yyourleng());
	    }
	}

	// Make new string with data up to the newline.
	size_t len = stop_at_eol ? (rtnp-m_lineChars.c_str()+1) : m_lineChars.length();
	string theLine(m_lineChars, 0, len);
	m_lineChars = m_lineChars.erase(0,len);	// Remove returned characters

	if (!m_preprocp->keepWhitespace() && !gotEof) {
	    const char* cp=theLine.c_str();
	    for (; *cp && (isspace(*cp) || *cp=='\n'); cp++) {}
	    if (!*cp) continue;
	}

	if (debug()) fprintf (stderr,"%d: GETLINE:  %s\n", m_filelinep->lineno(), theLine.c_str());
	return theLine;
    }
}