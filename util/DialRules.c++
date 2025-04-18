/*	$Id: DialRules.c++ 1058 2011-09-12 03:40:29Z faxguy $ */
/*
 * Copyright (c) 1993-1996 Sam Leffler
 * Copyright (c) 1993-1996 Silicon Graphics, Inc.
 * HylaFAX is a trademark of Silicon Graphics
 *
 * Permission to use, copy, modify, distribute, and sell this software and 
 * its documentation for any purpose is hereby granted without fee, provided
 * that (i) the above copyright notices and this permission notice appear in
 * all copies of the software and related documentation, and (ii) the names of
 * Sam Leffler and Silicon Graphics may not be used in any advertising or
 * publicity relating to the software without the specific, prior written
 * permission of Sam Leffler and Silicon Graphics.
 * 
 * THE SOFTWARE IS PROVIDED "AS-IS" AND WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS, IMPLIED OR OTHERWISE, INCLUDING WITHOUT LIMITATION, ANY 
 * WARRANTY OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.  
 * 
 * IN NO EVENT SHALL SAM LEFFLER OR SILICON GRAPHICS BE LIABLE FOR
 * ANY SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY KIND,
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER OR NOT ADVISED OF THE POSSIBILITY OF DAMAGE, AND ON ANY THEORY OF 
 * LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE 
 * OF THIS SOFTWARE.
 */

/*
 * HylaFAX Dialing String Rule Support.
 */
#include "Sys.h"
#include "DialRules.h"
#include "REArray.h"
#include "Dictionary.h"

#include <ctype.h>
#include <unistd.h>

/*
 * Interpolation points in replacement strings have
 * the META bit or'd together with their match identifier.
 * That is ``\1'' is META|1, ``\2'' is META|2, etc.
 * The ``&'' interpolation is treated as ``\0''.
 */
const u_int META = 0200;		// interpolation marker

struct DialRule {
    REPtr	pat;			// pattern to match
    fxStr	replace;		// replacement string
    fxStr	recurse;		// string is a recursive function

    DialRule();
    ~DialRule();

    int compare(const DialRule *) const;
};
DialRule::DialRule(){}
DialRule::~DialRule(){}
int DialRule::compare(const DialRule*) const { return 0; }

fxDECLARE_ObjArray(RuleArray, DialRule)
fxIMPLEMENT_ObjArray(RuleArray, DialRule)
fxDECLARE_StrKeyDictionary(VarDict, fxStr)
fxIMPLEMENT_StrKeyObjValueDictionary(VarDict, fxStr)
fxDECLARE_StrKeyDictionary(RulesDict, RuleArrayPtr)
fxIMPLEMENT_StrKeyObjValueDictionary(RulesDict, RuleArrayPtr)

DialStringRules::DialStringRules(const char* file) : filename(file)
{
    verbose = false;
    fp = NULL;
    vars = new VarDict;
    regex = new REArray;
    rules = new RulesDict;
}

DialStringRules::~DialStringRules()
{
    delete rules;
    delete regex;
    delete vars;
}

void DialStringRules::setVerbose(bool b) { verbose = b; }

/*
 * Parse the dialing string rules file
 * to create the in-memory rules.
 */
bool
DialStringRules::parse(bool shouldExist)
{
    bool ok = false;
    lineno = 0;
    fp = fopen(filename, "r");
    if (fp) {
	ok = parseRules();
	fclose(fp);
    } else if (shouldExist)
	parseError("Cannot open file \"%s\" for reading",
		(const char*) filename);
    return (ok);
}

void
DialStringRules::def(const fxStr& var, const fxStr& value)
{
    if (verbose)
	traceParse("Define %s = \"%s\"",
	    (const char*) var, (const char*) value);
    (*vars)[var] = value;
}

void
DialStringRules::undef(const fxStr& var)
{
    if (verbose)
	traceParse("Undefine %s", (const char*) var);
    vars->remove(var);
}

bool
DialStringRules::parseRules()
{
    char line[1024];
    char* cp;
    while ((cp = nextLine(line, sizeof (line)))) {
	// collect token
	if (!isalpha(*cp)) {
	    parseError("Syntax error, expecting identifier");
	    return (false);
	}
	const char* tp = cp;
	for (cp++; isalnum(*cp); cp++)
	    ;
	fxStr var(tp, cp-tp);
	while (isspace(*cp))
	    cp++;
	if (*cp == ':' && cp[1] == '=') {	// rule set definition
	    for (cp += 2; *cp != '['; cp++)
		if (*cp == '\0') {
		    parseError("Missing '[' while parsing rule set");
		    return (false);
		}
	    if (verbose)
		traceParse("%s := [", (const char*) var);
	    RuleArray* ra = new RuleArray;
	    if (!parseRuleSet(*ra)) {
		delete ra;
		return (false);
	    }
	    (*rules)[var] = ra;
	    if (verbose)
		traceParse("]");
	} else if (*cp == '=') {		// variable definition
	    fxStr value;
	    if (parseToken(cp+1, value) == NULL)
		return (false);
	    def(var, value);
	} else {				// an error
	    parseError("Missing '=' or ':=' after \"%s\"", (const char*) var);
	    return (false);
	}
    }
    if (verbose) {
	RuleArray* ra = (*rules)["CanonicalNumber"];
	if (ra == 0)
	    traceParse("Warning, no \"CanonicalNumber\" rules.");
	ra = (*rules)["DialString"];
	if (ra == 0)
	    traceParse("Warning, no \"DialString\" rules.");
    }
    return (true);
}

/*
 * Locate the next non-blank line in the file
 * and return a pointer to the first non-whitespace
 * character on the line.  Leading white space and
 * comments are stripped.  NULL is returned on EOF.
 */
char*
DialStringRules::nextLine(char* line, int lineSize)
{
    char* cp;
    do {
	if (!fgets(line, lineSize, fp))
	    return (NULL);
	lineno++;
	for (cp = line; (cp = strchr(cp, '!')); cp++)
	    if (cp == line || cp[-1] != '\\')
		break;
	if (cp)
	    *cp = '\0';
	else if ((cp = strchr(line, '\n')))
	    *cp = '\0';
	for (cp = line; isspace(*cp); cp++)
	    ;
    } while (*cp == '\0');
    return (cp);
}

/*
 * Parse the next token on the input line and return
 * the result in the string v.  Variable references
 * are interpolated here, though they might be better
 * done at a later time.
 */
const char*
DialStringRules::parseToken(const char* cp, fxStr& v)
{
    while (isspace(*cp))
	cp++;
    const char* tp;
    if (*cp == '"') {			// "..."
	tp = ++cp;
	for (;;) {
	    if (*cp == '\0') {
		parseError("String with unmatched '\"'");
		return (NULL);
	    }
	    if (*cp == '\\' && cp[1] == '\0') {
		parseError("Bad '\\' escape sequence");
		return (NULL);
	    }
	    if (*cp == '"' && (cp == tp || cp[-1] != '\\'))
		break;
	    cp++;
	}
	v = fxStr(tp, cp-tp);
	cp++;				// skip trailing ``"''
    } else {				// token terminated by white space
	for (tp = cp; *cp != '\0'; cp++) {
	    if (*cp == '\\' && cp[1] == '\0') {
		parseError("Bad '\\' escape sequence");
		return (NULL);
	    }
	    if (isspace(*cp) && (cp == tp || cp[-1] != '\\'))
		break;
	}
	v = fxStr(tp, cp-tp);
    }
    for (u_int i = 0, n = v.length(); i < n; i++) {
	if (v[i] == '$' && (i+1<n && v[i+1] == '{')) {
	    /*
	     * Handle variable reference.
	     */
	    u_int l = v.next(i, '}');
	    if (l >= v.length()) {
		parseError("Missing '}' for variable reference");
		return (NULL);
	    }
	    fxStr var = v.cut(i+2,l-(i+2));// variable name
	    v.remove(i, 3);		// remove ${}
	    const fxStr& value = (*vars)[var];
	    v.insert(value, i);		// insert variable's value
	    n = v.length();		// adjust string length
	    i += value.length()-1;	// don't scan substituted string
	} else if (v[i] == '\\')	// pass \<char> escapes unchanged
	    i++;
    }
    return cp;
}

/*
 * Handle \escapes for a token
 * that appears on the RHS of a rewriting rule.
 */
void
DialStringRules::subRHS(fxStr& v)
{
    /*
     * Replace `&' and \n items with (META|<n>) to mark spots
     * in the token where matches should be interpolated.
     */
    for (u_int i = 0, n = v.length(); i < n; i++) {
	if (v[i] == '\\') {		// process \<char> escapes
	    if (isdigit(v[i+1])) {
		v.remove(i), n--;
		v[i] = META | (v[i] - '0');
	    } else
		parseError("Bad '\\' escape sequence");
	} else if (v[i] == '&')
	    v[i] = META | 0;
    }
}

/*
 * Parse a set of rules and construct the array of
 * rules (regular expressions + replacement strings).
 */
bool
DialStringRules::parseRuleSet(RuleArray& rules)
{
    for (;;) {
	char line[1024];
	const char* cp = nextLine(line, sizeof (line));
	if (!cp) {
	    parseError("Missing ']' while parsing rule set");
	    return (false);
	}
	if (*cp == ']')
	    return (true);
	// new rule
	fxStr pat;
	if ((cp = parseToken(cp, pat)) == NULL)
	    return (false);
	while (isspace(*cp))
	    cp++;
	if (*cp != '=') {
	    parseError("Rule pattern without '='");
	    return (false);
	}
	DialRule r;
	if (parseToken(cp+1, r.replace) == NULL)
	    return (false);

	if (r.replace.length()> 2 && r.replace[0] == '\\' && r.replace[1] != '\\')
	{
	    u_int pos = 1;
	    fxStr name = r.replace.token(pos, "()");
	    if (pos < r.replace.length() && r.replace[pos-1] == '(' )
	    {
		pos += 1;
		fxStr data = r.replace.token(pos, "()");

		if (pos == r.replace.length() && r.replace[pos-1] == ')')
		{
		    r.recurse = name;
		    r.replace.resize(pos-1);
		    r.replace.remove(0,name.length()+2);
		}
	    }
	}

	if (verbose) {
	    if (r.recurse.length() == 0)
		traceParse("  \"%s\" = \"%s\"",
		    (const char*) pat, (const char*) r.replace);
	    else
		traceParse("  \"%s\" = Apply %s rules to (\"%s\")", (const char*)pat,
		    (const char*)r.recurse, (const char*)r.replace);
	}

	subRHS(r.replace);
	u_int i = 0;
	u_int n = regex->length();
	while (i < n) {
	    RE* re = (*regex)[i];
	    if (strcmp(re->pattern(), pat) == 0)
		break;
	    i++;
	}
	if (i >= n) {
	    r.pat = new RE(pat);
	    if (r.pat->getErrorCode() > REG_NOMATCH) {
		fxStr emsg;
		r.pat->getError(emsg);
		parseError(pat | ": " | emsg);
	    }
	    regex->append(r.pat);
	} else
	    r.pat = (*regex)[i];
	rules.append(r);
    }
}

static void
closeAllBut(int fd)
{
    for (int f = Sys::getOpenMax()-1; f >= 0; f--)
	if (f != fd)
	    Sys::close(f);
}

fxStr
DialStringRules::applyRules(const fxStr& name, const fxStr& s, u_int level)
{
    if (level >= 10) {
	traceRules("DialRules recursion depth reached: %u", level);
	return s;
    }
    fxStr prefix;
    for (u_int i = 0; i < level; i++)
	prefix.insert("    ");
    if (verbose)
	traceRules(prefix | "Apply %s rules to \"%s\"",
	    (const char*) name, (const char*) s);
    fxStr result(s);
    RuleArray* ra = (*rules)[name];
    if (ra) {
	for (u_int i = 0, n = (*ra).length(); i < n; i++) {
	    DialRule& rule = (*ra)[i];
	    u_int off = 0;
	    while (rule.pat->Find(result, off)) {
		/*
		 * Regular expression match.
		 */
		int ix = rule.pat->StartOfMatch();
		int len = rule.pat->EndOfMatch() - ix;
		if (len == 0)		// avoid looping on zero-length matches
		    break;
		/*
		 * Do ``&'' and ``\n'' interpolations in the
		 * replacement.  NB: & is treated as \0.
		 */
		fxStr replace(rule.replace);

		/*
		 * Perform external reference replacements on ~{...} segments.
		 */
		u_int extstart = replace.next(0, '~');
		if (extstart < replace.length()) {
		    u_int extobrace = replace.next(extstart, '{');
		    if (extobrace < replace.length() && extobrace == extstart+1) {
			u_int extcbrace = replace.next(extobrace, '}');
			if (extcbrace < replace.length()) {
			    fxStr extref = replace.cut(extstart+2, extcbrace-extobrace-1);
			    fxStr extinput = result.extract(ix, len);
			    replace.remove(extstart, 3);
			    const char *app[3];
			    app[0] = (const char*) extref;
			    app[1] = (const char*) extinput;
			    app[2] = NULL;
			    traceRules(prefix | "--> EXTERNAL: %s '%s'", app[0], app[1]);
			    int pfd[2], fd;
			    if (pipe(pfd) >= 0) {
				pid_t pid = fork();
				switch (pid) {
				    case -1:				// fork failure
					parseError("External fork failed, leaving unchanged.");
					replace.insert(extinput, extstart);
					Sys::close(pfd[1]);
					break;
				    case 0:				// child, exec command
					if (pfd[1] != STDOUT_FILENO)
					    dup2(pfd[1], STDOUT_FILENO);
					/*
					 * We're redirecting application stdout back through the pipe
					 * to the parent, but application stderr is not meaningful
					 * to faxq, and we don't want parent stdin to be available to the
					 * application, either.  However, some libc versions require
					 * stdin to not be closed, and some applications depend on stdin
					 * and stderr to be valid.  So we close all except for stdout,
					 * and then redirect stdin and stderr to devnull.
					 */
					closeAllBut(STDOUT_FILENO);
					fd = Sys::open(_PATH_DEVNULL, O_RDWR);
					if (fd != STDIN_FILENO)
					{
					    dup2(fd, STDIN_FILENO);
					    Sys::close(fd);
					}
					fd = Sys::open(_PATH_DEVNULL, O_RDWR);
					if (fd != STDERR_FILENO)
					{
					    dup2(fd, STDERR_FILENO);
					    Sys::close(fd);
					}
					Sys::execv(app[0], (char * const*)app);
					sleep(1);			//  give parent time to catch signal
					// We need to push extinput back through the pipe if execv fails.
					dprintf(STDOUT_FILENO, "%s", (const char*) extinput);
					_exit(255);
					/*NOTREACHED*/
				    default:			// parent, read from pipe and wait
					{
					    Sys::close(pfd[1]);
					    int estat = -1;
					    char data[64];
					    int n;
					    fxStr buf;
					    while ((n = Sys::read(pfd[0], data, sizeof(data))) > 0) {
						buf.append(data, n);
					    }
					    Sys::close(pfd[0]);
					    replace.insert(buf, extstart);
			                    (void) Sys::waitpid(pid, estat);
					}
					break;
				}
			    } else {
				// If our pipe fails, we can't run the child.
				replace.insert(extinput, extstart);
			    }
			}
		    }
		}

		for (u_int ri = 0, rlen = replace.length(); ri < rlen; ri++)
		    if (replace[ri] & META) {
			u_char mn = replace[ri] &~ META;
			int ms = rule.pat->StartOfMatch(mn);
			int mlen = rule.pat->EndOfMatch(mn) - ms;
			replace.remove(ri);	// delete & or \n
			if (mlen > 0)
			    replace.insert(result.extract(ms, mlen), ri);
			rlen = replace.length();// adjust string length ...
			ri += mlen - 1;		// ... and scan index
		    }
 		/*
		 * Run recursion on the replace part
		 */
		if (rule.recurse.length()) {
		    if (verbose)
			traceRules(prefix | "--> match rule \"%s\", Apply %s(\"%s\")",
			    rule.pat->pattern(), (const char*) rule.recurse, (const char*)replace);
		    fxStr recres = applyRules(rule.recurse, replace, level+1);
		    replace = recres;
		}
		/*
		 * Paste interpolated replacement string over match.
		 */
		result.remove(ix, len);
		result.insert(replace, ix);
		off = ix + replace.length();	// skip replace when searching
		if (verbose) {
		    traceRules(prefix | "--> match rule \"%s\", result now \"%s\"",
			rule.pat->pattern(), (const char*) result);
		}
	    }
	}
    }
    if (verbose)
	traceRules(prefix | "--> return result \"%s\"", (const char*) result);
    return result;
}

#include <stdarg.h>

void
DialStringRules::parseError(const char* fmt ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: line %u: ", (const char*) filename, lineno); 
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    putc('\n', stderr);
}

void
DialStringRules::traceParse(const char* fmt ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "%s: line %u: ", (const char*) filename, lineno); 
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    putc('\n', stdout);
}

void
DialStringRules::traceRules(const char* fmt ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    putc('\n', stdout);
}

/*
 * Apply the canonical number rule set to a string.
 */
fxStr DialStringRules::canonicalNumber(const fxStr& s)
    { return applyRules("CanonicalNumber", s); }
/*
 * Apply the dial string rule set to a string.
 */
fxStr DialStringRules::dialString(const fxStr& s)
    { return applyRules("DialString", s); }
/*
 * Apply the display number rule set to a string.
 */
fxStr DialStringRules::displayNumber(const fxStr& s)
    { return applyRules("DisplayNumber", s); }
