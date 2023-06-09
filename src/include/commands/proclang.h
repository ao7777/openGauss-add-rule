/*
 * src/include/commands/proclang.h
 *
 * -------------------------------------------------------------------------
 *
 * proclang.h
 *	  prototypes for proclang.c.
 *
 *
 * -------------------------------------------------------------------------
 */
#ifndef PROCLANG_H
#define PROCLANG_H

#include "nodes/parsenodes.h"

extern ObjectAddress CreateProceduralLanguage(CreatePLangStmt* stmt);
extern void DropProceduralLanguageById(Oid langOid);
extern ObjectAddress AlterLanguageOwner(const char* name, Oid newOwnerId);
extern void AlterLanguageOwner_oid(Oid oid, Oid newOwnerId);
extern bool PLTemplateExists(const char* languageName);
extern Oid get_language_oid(const char* langname, bool missing_ok);
extern char* get_language_name(Oid languageOid);
#endif /* PROCLANG_H */
