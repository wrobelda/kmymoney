/***************************************************************************
 *   Copyright 2009  Cristian Onet onet.cristian@gmail.com                 *
 *   Copyright 2004  Martin Preuss aquamaniac@users.sourceforge.net        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU General Public License as        *
 *   published by the Free Software Foundation; either version 2 of        *
 *   the License or (at your option) version 3 or any later version        *
 *   accepted by the membership of KDE e.V. (or its successor approved     *
 *   by the membership of KDE e.V.), which shall act as a proxy            *
 *   defined in Section 14 of version 3 of the license.                    *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>  *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
# include <config-kmymoney.h>
#endif


#include "kbanking.h"

#include <assert.h>
#include <qstring.h>
#include <qmessagebox.h>
#include <qinputdialog.h>
#include <qapplication.h>
#include <qdatetime.h>

#include <gwenhywfar/debug.h>
#include <gwenhywfar/text.h>

#include <qwidget.h>

KBanking::KBanking(const char *appname,
                   const char *cfgDir)
    : QBanking(appname, cfgDir)
    , _jobQueue(NULL)
{
}



KBanking::~KBanking()
{
}



int KBanking::init()
{
  int rv;

  rv = QBanking::init();
  if (rv < 0)
    return rv;

  rv = onlineInit();
  if (rv) {
    fprintf(stderr, "Error on online init (%d).\n", rv);
    QBanking::fini();
    return rv;
  }

  _jobQueue = AB_Job_List2_new();

#if AQB_IS_VERSION(4,0,0,2)
  /* since 4.0.0.2 the class QBGui does certificate handling itself */
#else
  GWEN_DB_NODE *dbCerts = 0;
# if AQB_IS_VERSION(3,9,0,0)
  rv = lockSharedConfig("certs", 0);
  if (rv < 0) {
    fprintf(stderr, "Could not lock certificate db (%d).\n", rv);
  } else {
    rv = loadSharedConfig("certs", &dbCerts, 0);
    if (rv < 0) {
      fprintf(stderr, "Could not load certificates (%d).\n", rv);
    }
    rv = unlockSharedConfig("certs", 0);
    if (rv < 0) {
      fprintf(stderr, "Could not unlock certificate db (%d).\n", rv);
    }
  }
# else
  dbCerts = AB_Banking_GetSharedData(getCInterface(), "certs");
# endif

  if (dbCerts)
    getGui()->setDbCerts(GWEN_DB_Group_dup(dbCerts));
#endif

  return 0;
}



int KBanking::fini()
{
  int rv;

  if (_jobQueue) {
    AB_Job_List2_FreeAll(_jobQueue);
    _jobQueue = NULL;
  }

#if AQB_IS_VERSION(4,0,0,2)
  /* since 4.0.0.2 the class QBGui does certificate handling itself */
#else
  GWEN_DB_NODE *dbCerts = 0;
  dbCerts = getGui()->getDbCerts();
# if AQB_IS_VERSION(3,9,0,0)
  if (dbCerts) {
    rv = lockSharedConfig("certs", 0);
    if (rv < 0) {
      fprintf(stderr, "Could not lock certificate db (%d).\n", rv);
    } else {
      rv = saveSharedConfig("certs", dbCerts, 0);
      if (rv < 0) {
        fprintf(stderr, "Could not load certificates (%d).\n", rv);
      }
      rv = unlockSharedConfig("certs", 0);
      if (rv < 0) {
        fprintf(stderr, "Could not unlock certificate db (%d).\n", rv);
      }
    }
  }
# else
  GWEN_DB_NODE *db2;
  db2 = AB_Banking_GetSharedData(getCInterface(), "certs");
  if (db2) {

    dbCerts = getGui()->getDbCerts();
    if (dbCerts) {
      GWEN_DB_ClearGroup(db2, 0);
      GWEN_DB_AddGroupChildren(db2, dbCerts);
    }
  }
# endif
#endif

  rv = onlineFini();
  if (rv) {
    QBanking::fini();
    return rv;
  }
  return QBanking::fini();
}



int KBanking::executeQueue(AB_IMEXPORTER_CONTEXT *ctx)
{
  int rv;
  AB_JOB_LIST2 *oldQ;

  rv = QBanking::executeJobs(_jobQueue, ctx, 0);
  oldQ = _jobQueue;
  _jobQueue = AB_Job_List2_new();
  flagStaff()->queueUpdated();
  AB_Job_List2_FreeAll(oldQ);
  return rv;
}



std::list<AB_JOB*> KBanking::getEnqueuedJobs()
{
  AB_JOB_LIST2 *ll;
  std::list<AB_JOB*> rl;

  ll = _jobQueue;
  if (ll && AB_Job_List2_GetSize(ll)) {
    AB_JOB *j;
    AB_JOB_LIST2_ITERATOR *it;

    it = AB_Job_List2_First(ll);
    assert(it);
    j = AB_Job_List2Iterator_Data(it);
    assert(j);
    while (j) {
      rl.push_back(j);
      j = AB_Job_List2Iterator_Next(it);
    }
    AB_Job_List2Iterator_free(it);
  }
  return rl;
}



int KBanking::enqueueJob(AB_JOB *j)
{
  assert(_jobQueue);
  assert(j);
  AB_Job_Attach(j);
  AB_Job_List2_PushBack(_jobQueue, j);
  flagStaff()->queueUpdated();
  return 0;
}



int KBanking::dequeueJob(AB_JOB *j)
{
  assert(_jobQueue);
  AB_Job_List2_Remove(_jobQueue, j);
  AB_Job_free(j);
  flagStaff()->queueUpdated();
  return 0;
}








