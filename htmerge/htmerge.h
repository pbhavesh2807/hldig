//
// htmerge.h
//
// htmerge: The interface to the htmerge program
//          Defines the calling conventions for
//            mergeDB -> db.cc (merging two databases)
//            mergeWords -> words.cc (updating the word db)
//            convertDocs -> docs.cc (updating the doc db)
//            reportError -> htmerge.cc (reporting errors)
//    
// Part of the ht://Dig package   <http://www.htdig.org/>
// Copyright (c) 1999 The ht://Dig Group
// For copyright details, see the file COPYING in your distribution
// or the GNU Public License version 2 or later
// <http://www.gnu.org/copyleft/gpl.html>
//
// $Id: htmerge.h,v 1.7 1999/09/08 17:19:39 loic Exp $
//
//
#ifndef _htmerge_h_
#define _htmerge_h_

#include "defaults.h"
#include "DocumentDB.h"
#include "HtURLCodec.h"
#include "WordList.h"
#include "WordRecord.h"
#include "htString.h"
#include <fstream.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>


// Globals shared by one or more components of htmerge
extern Dictionary	discard_list;
extern int		verbose;
extern int		stats;
extern Configuration	merge_config;


// Component procedures
void mergeDB();
void mergeWords();
void convertDocs();

// Of course reporting errors too
void reportError(char *msg);

#endif


