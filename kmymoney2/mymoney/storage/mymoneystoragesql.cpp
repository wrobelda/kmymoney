/***************************************************************************
                          mymoneystoragesql.cpp
                          ---------------------
    begin                : 11 November 2005
    copyright            : (C) 2005 by Tony Bloomfield
    email                : tonybloom@users.sourceforge.net
                         : Fernando Vilas <fvilas@iname.com>
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <algorithm>
#include <numeric>

// ----------------------------------------------------------------------------
// QT Includes

#include <QString>
#include <QDateTime>
#include <QStringList>
#include <QIODevice>
#include <QSqlQuery>
#include <QSqlError>
#include <QList>
#include <QSqlRecord>
#include <QMap>

// ----------------------------------------------------------------------------
// KDE Includes

#include <klocale.h>

// ----------------------------------------------------------------------------
// Project Includes

#include "mymoneystoragesql.h"
#include "imymoneyserialize.h"
#include <kmymoneyglobalsettings.h>


#define TRY try {
#define CATCH } catch (MyMoneyException *e) {
#define PASS } catch (MyMoneyException *e) { throw; }
#define ECATCH }
#define DBG(a) // qDebug(a)
//#define TRACE(a) qDebug(a)
#define TRACE(a) ::timetrace(a)

//***************** THE CURRENT VERSION OF THE DATABASE LAYOUT ****************
unsigned int MyMoneyDbDef::m_currentVersion = 6;

// subclass QSqlQuery for performance tracing
MyMoneySqlQuery::MyMoneySqlQuery (MyMoneyStorageSql*  db)
  : QSqlQuery (*db), m_db (db) {
}

MyMoneySqlQuery::~MyMoneySqlQuery () {
}

bool MyMoneySqlQuery::exec () {
  TRACE(QString("start sql - %1").arg(lastQuery()).toLatin1());
  bool rc = QSqlQuery::exec();
  QString msg("end sql\n%1\n***Query returned %2, row count %3");
  TRACE (msg.arg(QSqlQuery::executedQuery()).arg(rc).arg(numRowsAffected()).toLatin1());
  //DBG (QString("%1\n***Query returned %2, row count %3").arg(QSqlQuery::executedQuery()).arg(rc).arg(size()));
  return (rc);
}

bool MyMoneySqlQuery::prepare ( const QString & query ) {
  if (m_db->isSqlite3()) {
    QString newQuery = query;
    return (QSqlQuery::prepare (newQuery.replace("FOR UPDATE", "")));
  }
  return (QSqlQuery::prepare (query));
}

//*****************************************************************************
// Create a class to handle db transactions using scope
//
// Don't let the database object get destroyed while this object exists,
// that would result in undefined behavior.
class MyMoneyDbTransaction {
  public:
    MyMoneyDbTransaction(MyMoneyStorageSql& db, const QString& name)
      : m_db(db), m_name (name)
    { db.startCommitUnit(name); }

    ~MyMoneyDbTransaction() {
      if (std::uncaught_exception()) {
        m_db.cancelCommitUnit(m_name);
      } else {
        m_db.endCommitUnit(m_name);
      }
    }
  private:
    MyMoneyStorageSql& m_db;
    QString m_name;
};

//*****************************************************************************
MyMoneyDbDrivers::MyMoneyDbDrivers () {
  m_driverMap["QDB2"] = QString("IBM DB2");
  m_driverMap["QIBASE"] = QString("Borland Interbase");
  m_driverMap["QMYSQL3"] = QString("MySQL");
  m_driverMap["QOCI8"] = QString("Oracle Call Interface, version 8 and 9");
  m_driverMap["QODBC3"] = QString("Open Database Connectivity");
  m_driverMap["QPSQL7"] = QString("PostgreSQL v6.x and v7.x");
  m_driverMap["QTDS7"] = QString("Sybase Adaptive Server and Microsoft SQL Server");
  m_driverMap["QSQLITE"] = QString("SQLite Version 3");
}

databaseTypeE MyMoneyDbDrivers::driverToType (const QString& driver) const {
  if (driver == "QDB2") return(Db2);
  else if (driver == "QIBASE") return(Interbase);
  else if (driver == "QMYSQL3") return(Mysql);
  else if (driver == "QOCI8") return(Oracle8);
  else if (driver == "QODBC3") return(ODBC3);
  else if (driver == "QPSQL7") return(Postgresql);
  else if (driver == "QTDS7") return(Sybase);
  else if (driver == "QSQLITE") return(Sqlite3);
  else throw new MYMONEYEXCEPTION (QString("Unknown database driver type").arg(driver));
}

bool MyMoneyDbDrivers::isTested (databaseTypeE dbType) const {
  switch (dbType) {
    case Mysql:
    case Sqlite3:
    case Postgresql:
      return (true);
    default:
      return(false);
  }
  return(false);
}

//************************ Constructor/Destructor *****************************
MyMoneyStorageSql::MyMoneyStorageSql (IMyMoneySerialize *storage, const KUrl& url)
  : QSqlDatabase (url.queryItem("driver")) {
  DBG("*** Entering MyMoneyStorageSql::MyMoneyStorageSql");
  m_storage = storage;
  init();
}

void MyMoneyStorageSql::init(void) {
  m_storagePtr = dynamic_cast<IMyMoneyStorage*>(m_storage);
  m_dbVersion = 0;
  m_progressCallback = 0;
  m_displayStatus = false;
  m_readingPrices = false;
  m_newDatabase = false;
  m_loadAll = false;
  m_override = false;
  m_preferred.setReportAllSplits(false);
}

int MyMoneyStorageSql::open(const KUrl& url, int openMode, bool clear) {
  DBG("*** Entering MyMoneyStorageSql::open");
try {
  int rc = 0;
  QString driverName = url.queryItem("driver");
  m_dbType = m_drivers.driverToType(driverName);
  //get the input options
  QStringList options = url.queryItem("options").split(",");
  m_loadAll = options.contains("loadAll")/*|| m_mode == 0*/;
  m_override = options.contains("override");

  // create the database connection
  QString dbName = url.path().right(url.path().length() - 1); // remove separator slash
  setDatabaseName(dbName);
  setHostName(url.host());
  setUserName(url.user());
  setPassword(url.pass());
  switch (openMode) {
    case QIODevice::ReadOnly:    // OpenDatabase menu entry (or open last file)
    case QIODevice::ReadWrite:   // Save menu entry with database open
      if (!QSqlDatabase::open()) {
        buildError(MyMoneySqlQuery(this), __func__,  "opening database");
        rc = 1;
      } else {
        rc = createTables(); // check all tables are present, create if not (we may add tables at some time)
      }
      break;
    case QIODevice::WriteOnly:   // SaveAs Database - if exists, must be empty, if not will create
      // Try to open the database.
      // If that fails, try to create the database, then try to open it again.
      m_newDatabase = true;
      if (!QSqlDatabase::open()) {
        if (createDatabase(url) != 0) {
          rc = 1;
        } else {
          if (!QSqlDatabase::open()) {
            buildError(MyMoneySqlQuery(this), __func__, "opening new database");
            rc = 1;
          } else {
            rc = createTables();
          }
        }
      } else {
        rc = createTables();
        if (rc == 0) {
          if (clear) {
            clean();
          } else {
            rc = isEmpty();
          }
        }
      }
      break;
    default:
      qFatal("%s", qPrintable(QString("%1 - unknown open mode %2").arg(__func__).arg(openMode)));
  }
  if (rc != 0) return (rc);
  // bypass logon check if we are creating a database
  if (openMode == QIODevice::WriteOnly) return(0);
  // check if the database is locked, if not lock it
  readFileInfo();
  if (!m_logonUser.isEmpty() && (!m_override)) {
    m_error = i18n("Database apparently in use\nOpened by %1 on %2 at %3.\nOpen anyway?",
        m_logonUser,
        m_logonAt.date().toString(Qt::ISODate),
        m_logonAt.time().toString("hh.mm.ss"));
    qDebug("%s", qPrintable(m_error));
    close(false);
    rc = -1;
  } else {
    m_logonUser = url.user() + "@" + url.host();
    m_logonAt = QDateTime::currentDateTime();
    writeFileInfo();
  }
  return(rc);
} catch (QString& s) {
    qDebug("%s",qPrintable(s));
    return (1);
}
}

void MyMoneyStorageSql::close(bool logoff) {
  DBG("*** Entering MyMoneyStorageSql::close");
  if (QSqlDatabase::open()) {
    if (logoff) {
      MyMoneyDbTransaction t(*this, __func__);
      m_logonUser = QString();
      writeFileInfo();
    }
    QSqlDatabase::close();
    QSqlDatabase::removeDatabase(connectionName());
  }
}

int MyMoneyStorageSql::createDatabase (const KUrl& url) {
  DBG("*** Entering MyMoneyStorageSql::createDatabase");
  if (m_dbType == Sqlite3) return(0); // not needed for sqlite
  //if (!m_dbType == Mysql) { // in qt4 it doesn't work for mysql either
    m_error =i18n("Cannot currently create database for driver %1; please create manually",driverName());
    return (1);
  //}
  // create the database (only works for mysql at present)
  QString dbName = url.path().right(url.path().length() - 1); // remove separator slash
  QSqlDatabase maindb = QSqlDatabase::addDatabase(driverName(), "main");
  maindb.setDatabaseName ("mysql");
  maindb.setHostName (url.host());
  maindb.setUserName (url.user());
  maindb.setPassword (url.pass());
  QSqlQuery qm(maindb);
  if (!maindb.open()) {
      buildError (qm, __func__, "opening maindb", &maindb);
      return(1);
  }
  QString qs = QString("CREATE DATABASE %1;").arg(dbName);
  qm.prepare (qs);
  if (!qm.exec()) {
    buildError (qm, __func__,
               i18n("Error in create database %1; do you have create permissions?",dbName), &maindb);
    return (1);
  }
  maindb.close();
  QSqlDatabase::removeDatabase (maindb.connectionName());
  return (0);
}

int MyMoneyStorageSql::upgradeDb() {
  DBG("*** Entering MyMoneyStorageSql::upgradeDb");
  //signalProgress(0, 1, QObject::tr("Upgrading database..."));
  MyMoneySqlQuery q(this);
  q.prepare ("SELECT version FROM kmmFileInfo;");
  if (!q.exec() || !q.next()) {
    if (!m_newDatabase) {
      buildError (q, __func__, "Error retrieving file info(version)");
      return(1);
    } else {
      m_dbVersion = m_db.currentVersion();
      m_storage->setFileFixVersion(m_storage->currentFixVersion());
      QSqlQuery q(*this);
      q.prepare("UPDATE kmmFileInfo SET version = :version, \
                fixLevel = :fixLevel;");
      q.bindValue(":version", m_dbVersion);
      q.bindValue(":fixLevel", m_storage->currentFixVersion());
      if (!q.exec()) {
        buildError (q, __func__, "Error updating file info(version)");
        return(1);
      }
      return (0);
    }
  }
  // prior to dbv6, 'version' format was 'dbversion.fixLevel+1'
  // as of dbv6, these are separate fields
  QString version = q.value(0).toString();
  if (version.contains('.')) {
    m_dbVersion = q.value(0).toString().section('.', 0, 0).toUInt();
    m_storage->setFileFixVersion(q.value(0).toString().section('.', 1, 1).toUInt() - 1);
  } else {
    m_dbVersion = version.toUInt();
    q.prepare ("SELECT fixLevel FROM kmmFileInfo;");
    if (!q.exec() || !q.next()) {
      buildError (q, __func__, "Error retrieving file info (fixLevel)");
      return(1);
    }
    m_storage->setFileFixVersion(q.value(0).toUInt());  }
  int rc = 0;
  while ((m_dbVersion < m_db.currentVersion()) && (rc == 0)) {
    switch (m_dbVersion) {
    case 0:
      if ((rc = upgradeToV1()) != 0) return (1);
      ++m_dbVersion;
      break;
    case 1:
      if ((rc = upgradeToV2()) != 0) return (1);
      ++m_dbVersion;
      break;
    case 2:
      if ((rc = upgradeToV3()) != 0) return (1);
      ++m_dbVersion;
      break;
    case 3:
      if ((rc = upgradeToV4()) != 0) return (1);
      ++m_dbVersion;
      break;
    case 4:
      if ((rc = upgradeToV5()) != 0) return (1);
      ++m_dbVersion;
      break;
    case 5:
      if ((rc = upgradeToV6()) != 0) return (1);
      ++m_dbVersion;
      break;
    case 6:
      break;
    default:
      qFatal("Unknown version number in database - %d", m_dbVersion);
    }
  }
  // write updated version to DB
  //setVersion(QString("%1.%2").arg(m_dbVersion).arg(m_minorVersion))
  q.prepare (QString("UPDATE kmmFileInfo SET version = :version;"));
  q.bindValue(":version", m_dbVersion);
  if (!q.exec()) {
    buildError (q, __func__, "Error updating db version");
    return (1);
  }
  //signalProgress(-1,-1);
  return (0);
}
// SF bug 2779291
// check whether a column appears in a table already; if not, add it
bool MyMoneyStorageSql::addColumn
    (const QString& table, const QString& col,
                 const QString& after)
{
  MyMoneyDbTable t = m_db.m_tables[table];
  MyMoneyDbTable::field_iterator ft;
  KSharedPtr<MyMoneyDbColumn> c;
  for (ft = t.begin(); ft != t.end(); ++ft) {
    c = (*ft);
    if (c->name() == col)
      break;
  }
  if (ft == t.end()) qFatal("addColumn - get it right");
  return (addColumn(t, *(c.data()), after));
}

bool MyMoneyStorageSql::addColumn
    (const MyMoneyDbTable& t, const MyMoneyDbColumn& c,
                 const QString& after){
  if ((m_dbType == Sqlite3) && (!after.isEmpty()))
    qFatal("sqlite doesn't support 'AFTER'; use sqliteAlterTable");

  if (record(t.name()).contains(c.name()))
    return (true);

  MyMoneySqlQuery q(this);
  QString afterString = ";";
  if (!after.isEmpty())
    afterString = QString("AFTER %1;").arg(after);
  q.prepare("ALTER TABLE " + t.name() + " ADD COLUMN " +
                     c.generateDDL(m_dbType) + afterString);
  if (!q.exec()) {
    buildError (q, __func__,
      QString("Error adding column %1 to table %2").arg(c.name()).arg(t.name()));
    return (false);
  }
  return (true);
}

// analogous to above

// analogous to above
bool MyMoneyStorageSql::dropColumn
    (const QString& table, const QString& col)
{
  return (dropColumn(m_db.m_tables[table], col));
}

bool MyMoneyStorageSql::dropColumn
    (const MyMoneyDbTable& t, const QString& col){
  if (m_dbType == Sqlite3)
    qFatal("sqlite doesn't support 'DROP COLUMN'; use sqliteAlterTable");
  if (!record(t.name()).contains(col))
    return (true);
  MyMoneySqlQuery q(this);
  q.prepare("ALTER TABLE " + t.name() + " DROP COLUMN "
      + col + ";");
  if (!q.exec()) {
    buildError (q, __func__,
      QString("Error dropping column %1 from table %2").arg(col).arg(t.name()));
    return (false);
  }
  return (true);
}

int MyMoneyStorageSql::upgradeToV1() {
  DBG("*** Entering MyMoneyStorageSql::upgradeToV1");
  if ((m_dbType == Sqlite) || (m_dbType == Sqlite3)) qFatal("SQLite upgrade NYI");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  // change kmmSplits pkey to (transactionId, splitId)
  q.prepare ("ALTER TABLE kmmSplits ADD PRIMARY KEY (transactionId, splitId);");
  if (!q.exec()) {
    buildError (q, __func__, "Error updating kmmSplits pkey");
    return (1);
  }
  // change kmmSplits alter checkNumber varchar(32)
  q.prepare (m_db.m_tables["kmmSplits"].modifyColumnString(m_dbType, "checkNumber",
             MyMoneyDbColumn("checkNumber", "varchar(32)")));
  if (!q.exec()) {
    buildError (q, __func__, "Error expanding kmmSplits.checkNumber");
    return (1);
  }
  // change kmmSplits add postDate datetime
  if (!addColumn(m_db.m_tables["kmmSplits"],
            MyMoneyDbDatetimeColumn("postDate")))
    return (1);
  // initialize it to same value as transaction (do it the long way round)
  q.prepare ("SELECT id, postDate FROM kmmTransactions WHERE txType = 'N';");
  if (!q.exec()) {
    buildError (q, __func__, "Error priming kmmSplits.postDate");
    return (1);
  }
  QMap<QString, QDateTime> tids;
  while (q.next()) tids[q.value(0).toString()] = q.value(1).toDateTime();
  QMap<QString, QDateTime>::ConstIterator it;
  for (it = tids.constBegin(); it != tids.constEnd(); ++it) {
    q.prepare ("UPDATE kmmSplits SET postDate=:postDate WHERE transactionId = :id;");
    q.bindValue(":postDate", it.value().toString(Qt::ISODate));
    q.bindValue(":id", it.key());
    if (!q.exec()) {
      buildError (q, __func__, "priming kmmSplits.postDate");
      return(1);
    }
  }
  // add index to kmmKeyValuePairs to (kvpType,kvpId)
  QStringList list;
  list << "kvpType" << "kvpId";
  q.prepare (MyMoneyDbIndex("kmmKeyValuePairs", "kmmKVPtype_id", list, false).generateDDL(m_dbType) + ";");
  if (!q.exec()) {
      buildError (q, __func__, "Error adding kmmKeyValuePairs index");
      return (1);
  }
  // add index to kmmSplits to (accountId, txType)
  list.clear();
  list << "accountId" << "txType";
  q.prepare (MyMoneyDbIndex("kmmSplits", "kmmSplitsaccount_type", list, false).generateDDL(m_dbType) + ";");
  if (!q.exec()) {
    buildError (q, __func__, "Error adding kmmSplits index");
    return (1);
  }
  // change kmmSchedulePaymentHistory pkey to (schedId, payDate)
  q.prepare ("ALTER TABLE kmmSchedulePaymentHistory ADD PRIMARY KEY (schedId, payDate);");
  if (!q.exec()) {
    buildError (q, __func__, "Error updating kmmSchedulePaymentHistory pkey");
    return (1);
  }
      // change kmmPrices pkey to (fromId, toId, priceDate)
  q.prepare ("ALTER TABLE kmmPrices ADD PRIMARY KEY (fromId, toId, priceDate);");
  if (!q.exec()) {
    buildError (q, __func__, "Error updating kmmPrices pkey");
    return (1);
  }
  // change kmmReportConfig pkey to (name)
  // There wasn't one previously, so no need to drop it.
  q.prepare ("ALTER TABLE kmmReportConfig ADD PRIMARY KEY (name);");
  if (!q.exec()) {
    buildError (q, __func__, "Error updating kmmReportConfig pkey");
    return (1);
  }
  // change kmmFileInfo add budgets unsigned bigint after kvps
  if (!addColumn(m_db.m_tables["kmmFileInfo"],
            MyMoneyDbIntColumn("budgets", MyMoneyDbIntColumn::BIG, false)))
    return (1);
  // change kmmFileInfo add hiBudgetId unsigned bigint after hiReportId
  if (!addColumn(m_db.m_tables["kmmFileInfo"],
            MyMoneyDbIntColumn("hiBudgetId", MyMoneyDbIntColumn::BIG, false)))
    return (1);
      // change kmmFileInfo add logonUser
  if (!addColumn(m_db.m_tables["kmmFileInfo"],
            MyMoneyDbColumn("logonUser", "varchar(255)", false)))
    return (1);
      // change kmmFileInfo add logonAt datetime
  if (!addColumn(m_db.m_tables["kmmFileInfo"],
    MyMoneyDbDatetimeColumn("logonAt", false)))
    return (1);
      // change kmmAccounts add transactionCount unsigned bigint as last field
  if (!addColumn(m_db.m_tables["kmmAccounts"],
    MyMoneyDbIntColumn("transactionCount", MyMoneyDbIntColumn::BIG, false)))
    return (1);
  // calculate the transaction counts. the application logic defines an account's tx count
  // in such a way as to count multiple splits in a tx which reference the same account as one.
  // this is the only way I can think of to do this which will work in sqlite too.
  // inefficient, but it only gets done once...
  // get a list of all accounts so we'll get a zero value for those without txs
  q.prepare ("SELECT id FROM kmmAccounts");
  if (!q.exec()) {
    buildError (q, __func__, "Error retrieving accounts for transaction counting");
    return(1);
  }
  while (q.next()) {
    m_transactionCountMap[q.value(0).toString()] = 0;
  }
  q.prepare ("SELECT accountId, transactionId FROM kmmSplits WHERE txType = 'N' ORDER BY 1, 2");
  if (!q.exec()) {
    buildError (q, __func__, "Error retrieving splits for transaction counting");
    return(1);
  }
  QString lastAcc, lastTx;
  while (q.next()) {
    QString thisAcc = q.value(0).toString();
    QString thisTx = q.value(1).toString();
    if ((thisAcc != lastAcc) || (thisTx != lastTx)) ++m_transactionCountMap[thisAcc];
    lastAcc = thisAcc;
    lastTx = thisTx;
  }
  QMap<QString, unsigned long>::ConstIterator itm;
  q.prepare("UPDATE kmmAccounts SET transactionCount = :txCount WHERE id = :id;");
  for (itm = m_transactionCountMap.constBegin(); itm != m_transactionCountMap.constEnd(); ++itm) {
    q.bindValue (":txCount", QString::number(itm.value()));
    q.bindValue (":id", itm.key());
    if (!q.exec()) {
      buildError(q, __func__, "Error updating transaction count");
      return (1);
    }
  }
  m_transactionCountMap.clear();
  // there were considerable problems with record counts in V0, so rebuild them
  readFileInfo();
  m_institutions = getRecCount("kmmInstitutions");
  m_accounts = getRecCount("kmmAccounts");
  m_payees = getRecCount("kmmPayees");
  m_transactions = getRecCount("kmmTransactions WHERE txType = 'N'");
  m_splits = getRecCount("kmmSplits");
  m_securities = getRecCount("kmmSecurities");
  m_prices = getRecCount("kmmPrices");
  m_currencies = getRecCount("kmmCurrencies");
  m_schedules = getRecCount("kmmSchedules");
  m_reports = getRecCount("kmmReportConfig");
  m_kvps = getRecCount("kmmKeyValuePairs");
  m_budgets = getRecCount("kmmBudgetConfig");
  writeFileInfo();
  /* if sqlite {
    q.prepare("VACUUM;");
    if (!q.exec()) {
      buildError (q, __func__, "Error vacuuming database");
      return(1);
    }
  }*/
  return (0);
}

int MyMoneyStorageSql::upgradeToV2() {
  DBG("*** Entering MyMoneyStorageSql::upgradeToV2");
  //SQLite3 now supports ALTER TABLE...ADD COLUMN, so only die if version < 3
  //if (m_dbType == Sqlite3) qFatal("SQLite upgrade NYI");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  // change kmmSplits add price fields
  if (!addColumn(m_db.m_tables["kmmSplits"],
    MyMoneyDbTextColumn("price")))
    return (1);
  if (!addColumn(m_db.m_tables["kmmSplits"],
    MyMoneyDbTextColumn("priceFormatted")))
    return (1);
  return (0);
}

int MyMoneyStorageSql::upgradeToV3() {
  DBG("*** Entering MyMoneyStorageSql::upgradeToV3");
  //SQLite3 now supports ALTER TABLE...ADD COLUMN, so only die if version < 3
  //if (m_dbType == Sqlite3) qFatal("SQLite upgrade NYI");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  // The default value is given here to populate the column.
  q.prepare ("ALTER TABLE kmmSchedules ADD COLUMN " +
      MyMoneyDbIntColumn("occurenceMultiplier",
        MyMoneyDbIntColumn::SMALL, false, false, true)
        .generateDDL(m_dbType) + " DEFAULT 0;");
  if (!q.exec()) {
    buildError (q, __func__, "Error adding kmmSchedules.occurenceMultiplier");
    return (1);
  }
  //The default is less than any useful value, so as each schedule is hit, it will update
  //itself to the appropriate value.
  return 0;
}

int MyMoneyStorageSql::upgradeToV4() {
  DBG("*** Entering MyMoneyStorageSql::upgradeToV4");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  QStringList list;
  list << "transactionId" << "splitId";
  q.prepare (MyMoneyDbIndex("kmmSplits", "kmmTx_Split", list, false).generateDDL(m_dbType) + ";");
  if (!q.exec()) {
    buildError (q, __func__, "Error adding kmmSplits index on (transactionId, splitId)");
    return (1);
  }
  return 0;
}

int MyMoneyStorageSql::upgradeToV5() {
  DBG("*** Entering MyMoneyStorageSql::upgradeToV5");
  MyMoneyDbTransaction dbtrans(*this, __func__);
  MyMoneySqlQuery q(this);
  if (!addColumn(m_db.m_tables["kmmSplits"],
            MyMoneyDbTextColumn("bankId")))
    return (1);
  if (!addColumn(m_db.m_tables["kmmPayees"],
            MyMoneyDbTextColumn("notes", MyMoneyDbTextColumn::LONG)))
    return (1);
  if (!addColumn(m_db.m_tables["kmmPayees"],
            MyMoneyDbColumn("defaultAccountId", "varchar(32)")))
    return (1);
  if (!addColumn(m_db.m_tables["kmmPayees"],
    MyMoneyDbIntColumn("matchData", MyMoneyDbIntColumn::TINY,
                         false)))
    return (1);
  if (!addColumn(m_db.m_tables["kmmPayees"],
     MyMoneyDbColumn("matchIgnoreCase", "char(1)")))
    return (1);
  if (!addColumn(m_db.m_tables["kmmPayees"],
            MyMoneyDbTextColumn("matchKeys")))
    return (1);
  const MyMoneyDbTable& t = m_db.m_tables["kmmReportConfig"];
  if (m_dbType != Sqlite3) {
    q.prepare (t.dropPrimaryKeyString(m_dbType));
    if (!q.exec()) {
      buildError (q, __func__, "Error dropping Report table keys");
      return (1);
    }
  } else {
    if (!sqliteAlterTable(t))
      return (1);
  }
  return 0;
}

int MyMoneyStorageSql::upgradeToV6() {
  DBG("*** Entering MyMoneyStorageSql::upgradeToV6");
  startCommitUnit(__func__);
  MyMoneySqlQuery q(this);
  // add separate fix level in file info
  if (!addColumn("kmmFileInfo", "fixLevel"))
    return (1);
  // upgrade Mysql to InnoDB transaction-safe engine
  if (m_dbType == Mysql) {
    for (QMap<QString, MyMoneyDbTable>::ConstIterator tt = m_db.tableBegin(); tt != m_db.tableEnd(); ++tt) {
      q.prepare(QString("ALTER TABLE %1 ENGINE = InnoDB;").arg(tt.value().name()));
      if (!q.exec()) {
        buildError (q, __func__, "Error updating to InnoDB");
        return (1);
      }
    }
  }
  // add unique id to reports table
  if (!addColumn(m_db.m_tables["kmmReportConfig"],
       (MyMoneyDbColumn("id", "varchar(32)", false))))
    return(1);
  // read and write reports to get ids inserted
  readFileInfo();
  QMap<QString, MyMoneyReport> reportList =
      fetchReports();
  // the V5 database allowed lots of duplicate reports with no
  // way to distinguish between them. The fetchReports call
  // will have effectively removed all duplicates
  // so we now delete from the db and re-write them
  q.prepare("DELETE FROM kmmReportConfig;");
  if (!q.exec()) {
    buildError (q, __func__, "Error deleting reports");
    return (1);
  }
  unsigned long long hiReportId = 0;
  QMap<QString, MyMoneyReport>::const_iterator it_r;
  for(it_r = reportList.constBegin(); it_r != reportList.constEnd(); ++it_r) {
    MyMoneyReport r = *it_r;
    hiReportId = calcHighId(hiReportId, r.id());
    q.prepare (m_db.m_tables["kmmReportConfig"].insertString());
    writeReport(*it_r, q);
  }
  m_hiIdReports = hiReportId;
  m_storage->loadReportId(m_hiIdReports);
  // sqlite3 doesn't support ADD PRIMARY KEY
  if (m_dbType == Sqlite3) {
    if (!sqliteAlterTable(m_db.m_tables["kmmReportConfig"])) {
      return (1);
    }
  } else {
    q.prepare ("ALTER TABLE kmmReportConfig ADD PRIMARY KEY (id);");
    if (!q.exec()) {
      buildError (q, __func__, "Error updating kmmReportConfig pkey");
      return (1);
    }
  }
  endCommitUnit(__func__);
  return 0;
}

/* This function attempts to cater for limitations in the sqlite ALTER TABLE
   statement. It should enable us to drop a primary key, and drop columns */
bool MyMoneyStorageSql::sqliteAlterTable(const MyMoneyDbTable& t) {
  DBG("*** Entering MyMoneyStorageSql::sqliteAlterTable");
  QString tempTableName = t.name();
  tempTableName.replace("kmm", "tmp");
  MyMoneySqlQuery q(this);
  q.prepare (QString("ALTER TABLE " + t.name() + " RENAME TO " + tempTableName + ";"));
  if (!q.exec()) {
    buildError (q, __func__, "Error renaming table");
    return false;
  }
  createTable(t);
  q.prepare (QString("INSERT INTO " + t.name() + " (" + t.columnList() +
      ") SELECT " + t.columnList() + " FROM " + tempTableName + ";"));
  if (!q.exec()) {
    buildError (q, __func__, "Error inserting into new table");
    return false;
  }
  q.prepare (QString("DROP TABLE " + tempTableName + ";"));
  if (!q.exec()) {
    buildError (q, __func__, "Error dropping old table");
    return false;
  }
  return true;
}

long unsigned MyMoneyStorageSql::getRecCount (const QString& table) const {
  DBG("*** Entering MyMoneyStorageSql::getRecCount");
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  q.prepare(QString("SELECT COUNT(*) FROM %1;").arg(table));
  if ((!q.exec()) || (!q.next())) {
    buildError (q, __func__, "error retrieving record count");
    qFatal("Error retrieving record count"); // definitely shouldn't happen
  }
  return ((unsigned long) q.value(0).toULongLong());
}

int MyMoneyStorageSql::createTables () {
  DBG("*** Entering MyMoneyStorageSql::createTables");
  // check tables, create if required
  // convert everything to lower case, since SQL standard is case insensitive
  // table and column names (when not delimited), but some DBMSs disagree.
  QStringList lowerTables = tables(QSql::AllTables);
  for (QStringList::iterator i = lowerTables.begin(); i != lowerTables.end(); ++i) {
    (*i) = (*i).toLower();
  }

  for (QMap<QString, MyMoneyDbTable>::ConstIterator tt = m_db.tableBegin(); tt != m_db.tableEnd(); ++tt) {
    if (!lowerTables.contains(tt.key().toLower())) {
      createTable (tt.value());
    }
  }

  MyMoneySqlQuery q(this);
  for (QMap<QString, MyMoneyDbView>::ConstIterator tt = m_db.viewBegin(); tt != m_db.viewEnd(); ++tt) {
    if (!lowerTables.contains(tt.key().toLower())) {
      q.prepare (tt.value().createString());
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString ("creating view %1").arg(tt.key())));
    }
  }

  // get the current db version from kmmFileInfo.
  // upgrade if necessary.

  return (upgradeDb()); // any errors will be caught by exception handling
}

void MyMoneyStorageSql::createTable (const MyMoneyDbTable& t) {
  DBG("*** Entering MyMoneyStorageSql::createTable");
// create the tables
  QStringList ql = t.generateCreateSQL(m_dbType).split('\n', QString::SkipEmptyParts);
  MyMoneySqlQuery q(this);
  for (int i = 0; i < ql.count(); ++i) {
    q.prepare (ql[i]);
    if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString ("creating table/index %1").arg(t.name())));
  }
}

int MyMoneyStorageSql::isEmpty () {
  DBG("*** Entering MyMoneyStorageSql::isEmpty");
  // check all tables are empty
  QMap<QString, MyMoneyDbTable>::ConstIterator tt = m_db.tableBegin();
  int recordCount = 0;
  MyMoneySqlQuery q(this);
  while ((tt != m_db.tableEnd()) && (recordCount == 0)) {
    q.prepare (QString("select count(*) from %1;").arg((*tt).name()));
    if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "getting record count"));
    if (!q.next()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "retrieving record count"));
    recordCount += q.value(0).toInt();
    ++tt;
  }

  if (recordCount != 0) {
    return (-1); // not empty
  } else {
    return (0);
  }
}

void MyMoneyStorageSql::clean() {
  DBG("*** Entering MyMoneyStorageSql::clean");
// delete all existing records
  QMap<QString, MyMoneyDbTable>::ConstIterator it = m_db.tableBegin();
  MyMoneySqlQuery q(this);
  while (it != m_db.tableEnd()) {
    q.prepare(QString("DELETE from %1;").arg(it.key()));
    if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString ("cleaning database")));
    ++it;
  }
}

//////////////////////////////////////////////////////////////////

bool MyMoneyStorageSql::readFile(void) {
  DBG("*** Entering MyMoneyStorageSql::readFile");
  m_displayStatus = true;
  try {
    readFileInfo();
    readInstitutions();
    if (m_loadAll) {
      readPayees();
    } else {
      QList<QString> user;
      user.append(QString("USER"));
      readPayees(user);
    }
  //TRACE("done payees");
    readCurrencies();
  //TRACE("done currencies");
    readSecurities();
  //TRACE("done securities");
    readAccounts();
    if (m_loadAll) {
      readTransactions();
    } else {
      if (m_preferred.filterSet().singleFilter.accountFilter) readTransactions (m_preferred);
    }
  //TRACE("done accounts");
    readSchedules();
  //TRACE("done schedules");
    readPrices();
  //TRACE("done prices");
    readReports();
  //TRACE("done reports");
    readBudgets();
  //TRACE("done budgets");
   //FIXME - ?? if (m_mode == 0)
      //m_storage->rebuildAccountBalances();
  // this seems to be nonsense, but it clears the dirty flag
  // as a side-effect.
    m_storage->setLastModificationDate(m_storage->lastModificationDate());
    // FIXME?? if (m_mode == 0) m_storage = NULL;
  // make sure the progress bar is not shown any longer
    signalProgress(-1, -1);
    m_displayStatus = false;
    //MyMoneySqlQuery::traceOn();
    return true;
  } catch (QString& s) {
    return false;
  }
}

// The following is called from 'SaveAsDatabase'
bool MyMoneyStorageSql::writeFile(void) {
  DBG("*** Entering MyMoneyStorageSql::writeFile");
  // initialize record counts and hi ids
  m_institutions = m_accounts = m_payees = m_transactions = m_splits
      = m_securities = m_prices = m_currencies = m_schedules  = m_reports = m_kvps = m_budgets = 0;
  m_hiIdInstitutions = m_hiIdPayees = m_hiIdAccounts = m_hiIdTransactions =
      m_hiIdSchedules = m_hiIdSecurities = m_hiIdReports = m_hiIdBudgets = 0;
  m_displayStatus = true;
  try{
  MyMoneyDbTransaction t(*this, __func__);
  writeInstitutions ();
  writePayees();
  writeAccounts();
  writeTransactions();
  writeSchedules();
  writeSecurities();
  writePrices();
  writeCurrencies();
  writeReports();
  writeBudgets();
  writeFileInfo();
  // this seems to be nonsense, but it clears the dirty flag
  // as a side-effect.
  //m_storage->setLastModificationDate(m_storage->lastModificationDate());
  // FIXME?? if (m_mode == 0) m_storage = NULL;

  // make sure the progress bar is not shown any longer
  signalProgress(-1, -1);
  m_displayStatus = false;
  return true;
} catch (QString& s) {
  return false;
}
}
// --------------- SQL Transaction (commit unit) handling -----------------------------------
void MyMoneyStorageSql::startCommitUnit (const QString& callingFunction) {
  DBG("*** Entering MyMoneyStorageSql::startCommitUnit");
  if (m_commitUnitStack.isEmpty()) {
    if (!transaction()) throw new MYMONEYEXCEPTION(buildError (MyMoneySqlQuery(), callingFunction, "starting commit unit") + " " + callingFunction);
  }
  m_commitUnitStack.push(callingFunction);
}

bool MyMoneyStorageSql::endCommitUnit (const QString& callingFunction) {
  DBG("*** Entering MyMoneyStorageSql::endCommitUnit");
  // for now, we don't know if there were any changes made to the data so
  // we expect the data to have changed. This assumption causes some unnecessary
  // repaints of the UI here and there, but for now it's ok. If we can determine
  // that the commit() really changes the data, we can return that information
  // as value of this method.
  bool rc = true;
  if (m_commitUnitStack.isEmpty()) {
    throw new MYMONEYEXCEPTION("Empty commit unit stack while trying to commit");
  }

  if (callingFunction != m_commitUnitStack.top())
    qDebug("%s", qPrintable(QString("%1 - %2 s/be %3").arg(__func__).arg(callingFunction).arg(m_commitUnitStack.top())));
  m_commitUnitStack.pop();
  if (m_commitUnitStack.isEmpty()) {
    //qDebug() << "Committing with " << MyMoneySqlQuery::refCount() << " queries";
    if (!commit()) throw new MYMONEYEXCEPTION(buildError (QSqlQuery(), callingFunction, "ending commit unit"));
  }
  return rc;
}

void MyMoneyStorageSql::cancelCommitUnit (const QString& callingFunction) {
  DBG("*** Entering MyMoneyStorageSql::cancelCommitUnit");
  if (m_commitUnitStack.isEmpty()) return;
  if (callingFunction != m_commitUnitStack.top())
    qDebug("%s", qPrintable(QString("%1 - %2 s/be %3").arg(__func__).arg(callingFunction).arg(m_commitUnitStack.top())));
  m_commitUnitStack.clear();
  if (!rollback()) throw new MYMONEYEXCEPTION(buildError (MyMoneySqlQuery(), callingFunction, "cancelling commit unit") + " " + callingFunction);
}

/////////////////////////////////////////////////////////////////////
void MyMoneyStorageSql::fillStorage() {
  DBG("*** Entering MyMoneyStorageSql::fillStorage");
//  if (!m_transactionListRead)  // make sure we have loaded everything
    readTransactions();
//  if (!m_payeeListRead)
    readPayees();
}

//------------------------------ Write SQL routines ----------------------------------------
// **** Institutions ****
void MyMoneyStorageSql::writeInstitutions() {
  DBG("*** Entering MyMoneyStorageSql::writeInstitutions");
  // first, get a list of what's on the database
  // anything not in the list needs to be inserted
  // anything which is will be updated and removed from the list
  // anything left over at the end will need to be deleted
  // this is an expensive and inconvenient way to do things; find a better way
  // one way would be to build the lists when reading the db
  // unfortunately this object does not persist between read and write
  // it would also be nice if we could tell which objects had been updated since we read them in
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  q.prepare("SELECT id FROM kmmInstitutions;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Institution list"));
  while (q.next()) dbList.append(q.value(0).toString());

  const QList<MyMoneyInstitution> list = m_storage->institutionList();
  QList<MyMoneyInstitution>::ConstIterator it;
  MyMoneySqlQuery q2(this);
  q.prepare (m_db.m_tables["kmmInstitutions"].updateString());
  q2.prepare (m_db.m_tables["kmmInstitutions"].insertString());
  signalProgress(0, list.count(), "Writing Institutions...");
  for(it = list.begin(); it != list.end(); ++it) {
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      writeInstitution(*it, q);
    } else {
      writeInstitution(*it, q2);
    }
    signalProgress (++m_institutions, 0);
  }

  if (!dbList.isEmpty()) {
    QList<QString>::const_iterator it = dbList.constBegin();
    q.prepare("DELETE FROM kmmInstitutions WHERE id = :id");
    while (it != dbList.constEnd()) {
      q.bindValue(":id", (*it));
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Institution"));
      deleteKeyValuePairs("OFXSETTINGS", (*it));
      ++it;
    }
  }
}

void MyMoneyStorageSql::addInstitution(const MyMoneyInstitution& inst) {
  DBG("*** Entering MyMoneyStorageSql::addInstitution");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmInstitutions"].insertString());
  writeInstitution(inst ,q);
  ++m_institutions;
  writeFileInfo();
}

void MyMoneyStorageSql::modifyInstitution(const MyMoneyInstitution& inst) {
  DBG("*** Entering MyMoneyStorageSql::modifyInstitution");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmInstitutions"].updateString());
  deleteKeyValuePairs("OFXSETTINGS", inst.id());
  writeInstitution(inst ,q);
  writeFileInfo();
}

void MyMoneyStorageSql::removeInstitution(const MyMoneyInstitution& inst) {
  DBG("*** Entering MyMoneyStorageSql::removeInstitution");
  MyMoneyDbTransaction t(*this, __func__);
  deleteKeyValuePairs("OFXSETTINGS", inst.id());
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmInstitutions"].deleteString());
  q.bindValue(":id", inst.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting  Institution")));
  --m_institutions;
  writeFileInfo();
}

void MyMoneyStorageSql::writeInstitution(const MyMoneyInstitution& i, MyMoneySqlQuery& q) {
  DBG("*** Entering MyMoneyStorageSql::writeInstitution");
  q.bindValue(":id", i.id());
  q.bindValue(":name", i.name());
  q.bindValue(":manager", i.manager());
  q.bindValue(":routingCode", i.sortcode());
  q.bindValue(":addressStreet", i.street());
  q.bindValue(":addressCity", i.city());
  q.bindValue(":addressZipcode", i.postcode());
  q.bindValue(":telephone", i.telephone());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Institution")));
  writeKeyValuePairs("OFXSETTINGS", i.id(), i.pairs());
  m_hiIdInstitutions = calcHighId(m_hiIdInstitutions, i.id());
}

// **** Payees ****
void MyMoneyStorageSql::writePayees() {
  DBG("*** Entering MyMoneyStorageSql::writePayees");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  q.prepare("SELECT id FROM kmmPayees;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Payee list"));
  while (q.next()) dbList.append(q.value(0).toString());

  QList<MyMoneyPayee> list = m_storage->payeeList();
  MyMoneyPayee user(QString("USER"), m_storage->user());
  list.prepend(user);
  signalProgress(0, list.count(), "Writing Payees...");
  MyMoneySqlQuery q2(this);
  q.prepare (m_db.m_tables["kmmPayees"].updateString());
  q2.prepare (m_db.m_tables["kmmPayees"].insertString());
  QList<MyMoneyPayee>::ConstIterator it;
  for(it = list.constBegin(); it != list.constEnd(); ++it) {
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      writePayee(*it, q);
    } else {
      writePayee(*it, q2);
    }
    signalProgress(++m_payees, 0);
  }

  if (!dbList.isEmpty()) {
    QList<QString>::const_iterator it = dbList.constBegin();
    q.prepare(m_db.m_tables["kmmPayees"].deleteString());
    while (it != dbList.constEnd()) {
      q.bindValue(":id", (*it));
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Payee"));
      m_payees -= q.numRowsAffected();
      ++it;
    }
  }
}

void MyMoneyStorageSql::addPayee(const MyMoneyPayee& payee) {
  DBG("*** Entering MyMoneyStorageSql::addPayee");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmPayees"].insertString());
  writePayee(payee,q);
  ++m_payees;
  writeFileInfo();
}

void MyMoneyStorageSql::modifyPayee(const MyMoneyPayee& payee) {
  DBG("*** Entering MyMoneyStorageSql::modifyPayee");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmPayees"].updateString());
  writePayee(payee,q);
  writeFileInfo();
}

void MyMoneyStorageSql::modifyUserInfo(const MyMoneyPayee& payee) {
  DBG("*** Entering MyMoneyStorageSql::modifyUserInfo");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmPayees"].updateString());
  writePayee(payee,q, true);
  writeFileInfo();
}

void MyMoneyStorageSql::removePayee(const MyMoneyPayee& payee) {
  DBG("*** Entering MyMoneyStorageSql::removePayee");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmPayees"].deleteString());
  q.bindValue(":id", payee.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting  Payee")));
  --m_payees;
  writeFileInfo();
}

void MyMoneyStorageSql::writePayee(const MyMoneyPayee& p, MyMoneySqlQuery& q, bool isUserInfo) {
  DBG("*** Entering MyMoneyStorageSql::writePayee");
  if (isUserInfo) {
    q.bindValue(":id", "USER");
  } else {
    q.bindValue(":id", p.id());
  }
  q.bindValue(":name", p.name());
  q.bindValue(":reference", p.reference());
  q.bindValue(":email", p.email());
  q.bindValue(":addressStreet", p.address());
  q.bindValue(":addressCity", p.city());
  q.bindValue(":addressZipcode", p.postcode());
  q.bindValue(":addressState", p.state());
  q.bindValue(":telephone", p.telephone());
  q.bindValue(":notes", p.notes());
  q.bindValue(":defaultAccountId", p.defaultAccountId());
  bool ignoreCase;
  QString matchKeys;
  MyMoneyPayee::payeeMatchType type = p.matchData(ignoreCase, matchKeys);
  q.bindValue(":matchData", static_cast<unsigned int>(type));
  if (ignoreCase) q.bindValue(":matchIgnoreCase", "Y");
  else q.bindValue(":matchIgnoreCase", "N");
  q.bindValue(":matchKeys", matchKeys);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString ("writing Payee")));
  if (!isUserInfo) m_hiIdPayees = calcHighId(m_hiIdPayees, p.id());
}

// **** Accounts ****
void MyMoneyStorageSql::writeAccounts() {
  DBG("*** Entering MyMoneyStorageSql::writeAccounts");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  q.prepare("SELECT id FROM kmmAccounts;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Account list"));
  while (q.next()) dbList.append(q.value(0).toString());

  QList<MyMoneyAccount> list;
  m_storage->accountList(list);
  QList<MyMoneyAccount>::ConstIterator it;
  signalProgress(0, list.count(), "Writing Accounts...");
  if (dbList.isEmpty()) { // new table, insert standard accounts
    q.prepare (m_db.m_tables["kmmAccounts"].insertString());
  } else {
    q.prepare (m_db.m_tables["kmmAccounts"].updateString());
  }
  // Attempt to write the standard accounts. For an empty db, this will fail.
  TRY
    writeAccount(m_storage->asset(), q); ++m_accounts;
    writeAccount(m_storage->liability(), q); ++m_accounts;
    writeAccount(m_storage->expense(), q); ++m_accounts;
    writeAccount(m_storage->income(), q); ++m_accounts;
    writeAccount(m_storage->equity(), q); ++m_accounts;
  CATCH
    delete e;

    // If the above failed, assume that the database is empty and create
    // the standard accounts by hand before writing them.
    MyMoneyAccount acc_l;
    acc_l.setAccountType(MyMoneyAccount::Liability);
    acc_l.setName("Liability");
    MyMoneyAccount liability(STD_ACC_LIABILITY, acc_l);

    MyMoneyAccount acc_a;
    acc_a.setAccountType(MyMoneyAccount::Asset);
    acc_a.setName("Asset");
    MyMoneyAccount asset(STD_ACC_ASSET, acc_a);

    MyMoneyAccount acc_e;
    acc_e.setAccountType(MyMoneyAccount::Expense);
    acc_e.setName("Expense");
    MyMoneyAccount expense(STD_ACC_EXPENSE, acc_e);

    MyMoneyAccount acc_i;
    acc_i.setAccountType(MyMoneyAccount::Income);
    acc_i.setName("Income");
    MyMoneyAccount income(STD_ACC_INCOME, acc_i);

    MyMoneyAccount acc_q;
    acc_q.setAccountType(MyMoneyAccount::Equity);
    acc_q.setName("Equity");
    MyMoneyAccount equity(STD_ACC_EQUITY, acc_q);

    writeAccount(asset, q); ++m_accounts;
    writeAccount(expense, q); ++m_accounts;
    writeAccount(income, q); ++m_accounts;
    writeAccount(liability, q); ++m_accounts;
    writeAccount(equity, q); ++m_accounts;
  ECATCH

  int i = 0;
  MyMoneySqlQuery q2(this);
  q.prepare (m_db.m_tables["kmmAccounts"].updateString());
  q2.prepare (m_db.m_tables["kmmAccounts"].insertString());
  // Update the accounts that exist; insert the ones that do not.
  for(it = list.constBegin(); it != list.constEnd(); ++it, ++i) {
    m_transactionCountMap[(*it).id()] = m_storagePtr->transactionCount((*it).id());
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      writeAccount(*it, q);
    } else {
      writeAccount(*it, q2);
    }
    signalProgress(++m_accounts, 0);
  }

  // Delete the accounts that are in the db but no longer in memory.
  if (!dbList.isEmpty()) {
    QList<QString>::const_iterator it = dbList.constBegin();
    q.prepare("DELETE FROM kmmAccounts WHERE id = :id");
    while (it != dbList.constEnd()) {
      if (!m_storagePtr->isStandardAccount(*it)) {
        q.bindValue(":id", (*it));
        if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Account"));
        deleteKeyValuePairs("ACCOUNT", (*it));
        deleteKeyValuePairs("ONLINEBANKING", (*it));
      }
      ++it;
    }
  }
}

void MyMoneyStorageSql::addAccount(const MyMoneyAccount& acc) {
  DBG("*** Entering MyMoneyStorageSql::addAccount");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmAccounts"].insertString());
  writeAccount(acc,q);
  ++m_accounts;
  writeFileInfo();
}

void MyMoneyStorageSql::modifyAccount(const MyMoneyAccount& acc) {
  DBG("*** Entering MyMoneyStorageSql::modifyAccount");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmAccounts"].updateString());
  deleteKeyValuePairs("ACCOUNT", acc.id());
  deleteKeyValuePairs("ONLINEBANKING", acc.id());
  writeAccount(acc,q);
  writeFileInfo();
}

void MyMoneyStorageSql::removeAccount(const MyMoneyAccount& acc) {
  DBG("*** Entering MyMoneyStorageSql::removeAccount");
  MyMoneyDbTransaction t(*this, __func__);
  deleteKeyValuePairs("ACCOUNT", acc.id());
  deleteKeyValuePairs("ONLINEBANKING", acc.id());
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmAccounts"].deleteString());
  q.bindValue(":id", acc.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting Account")));
  --m_accounts;
  writeFileInfo();
}

void MyMoneyStorageSql::writeAccount(const MyMoneyAccount& acc, MyMoneySqlQuery& q) {
  DBG("*** Entering MyMoneyStorageSql::writeAccount");
  //MyMoneyMoney balance = m_storagePtr->balance(acc.id(), QDate());
  q.bindValue(":id", acc.id());
  q.bindValue(":institutionId", acc.institutionId());
  q.bindValue(":parentId", acc.parentAccountId());
  if (acc.lastReconciliationDate() == QDate())
    q.bindValue(":lastReconciled", acc.lastReconciliationDate());
  else
    q.bindValue(":lastReconciled", acc.lastReconciliationDate().toString(Qt::ISODate));

  q.bindValue(":lastModified", acc.lastModified());
  if (acc.openingDate() == QDate())
    q.bindValue(":openingDate", acc.openingDate());
  else
    q.bindValue(":openingDate", acc.openingDate().toString(Qt::ISODate));

  q.bindValue(":accountNumber", acc.number());
  q.bindValue(":accountType", acc.accountType());
  q.bindValue(":accountTypeString", MyMoneyAccount::accountTypeToString(acc.accountType()));
  if (acc.accountType() == MyMoneyAccount::Stock) {
    q.bindValue(":isStockAccount", "Y");
  } else {
    q.bindValue(":isStockAccount", "N");
  }
  q.bindValue(":accountName", acc.name());
  q.bindValue(":description", acc.description());
  q.bindValue(":currencyId", acc.currencyId());

  // This section attempts to get the balance from the database, if possible
  // That way, the balance fields are kept in sync. If that fails, then
  // It is assumed that the account actually knows its correct balance.

  //FIXME: Using exceptions for branching always feels like a kludge.
  //       Look for a better way.
  TRY
    MyMoneyMoney bal = m_storagePtr->balance(acc.id(), QDate());
    q.bindValue(":balance", bal.toString());
    q.bindValue(":balanceFormatted",
                bal.formatMoney("", -1, false));
  CATCH
    delete e;
    q.bindValue(":balance", acc.balance().toString());
    q.bindValue(":balanceFormatted",
                acc.balance().formatMoney("", -1, false));
  ECATCH

  q.bindValue(":transactionCount", Q_ULLONG(m_transactionCountMap[acc.id()]));
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Account")));

  //Add in Key-Value Pairs for accounts.
  //MMAccount inherits from KVPContainer AND has a KVPContainer member
  //so handle both
  writeKeyValuePairs("ACCOUNT", acc.id(), acc.pairs());
  writeKeyValuePairs("ONLINEBANKING", acc.id(), acc.onlineBankingSettings().pairs());
  m_hiIdAccounts = calcHighId(m_hiIdAccounts, acc.id());
}

// **** Transactions and Splits ****
void MyMoneyStorageSql::writeTransactions() {
  DBG("*** Entering MyMoneyStorageSql::writeTransactions");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  q.prepare("SELECT id FROM kmmTransactions WHERE txType = 'N';");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Transaction list"));
  while (q.next()) dbList.append(q.value(0).toString());

  MyMoneyTransactionFilter filter;
  filter.setReportAllSplits(false);
  QList<MyMoneyTransaction> list;
  m_storage->transactionList(list, filter);
  signalProgress(0, list.count(), "Writing Transactions...");
  QList<MyMoneyTransaction>::ConstIterator it;
  int i = 0;
  MyMoneySqlQuery q2(this);
  q.prepare (m_db.m_tables["kmmTransactions"].updateString());
  q2.prepare (m_db.m_tables["kmmTransactions"].insertString());
  for(it = list.constBegin(); it != list.constEnd(); ++it, ++i) {
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      writeTransaction((*it).id(), *it, q, "N");
    } else {
      writeTransaction((*it).id(), *it, q2, "N");
    }
    signalProgress(++m_transactions, 0);
  }

  if (!dbList.isEmpty()) {
    QList<QString>::const_iterator it = dbList.constBegin();
    while (it != dbList.constEnd()) {
      deleteTransaction(*it);
      ++it;
    }
  }
}

void MyMoneyStorageSql::addTransaction (const MyMoneyTransaction& tx) {
  DBG("*** Entering MyMoneyStorageSql::addTransaction");
  MyMoneyDbTransaction t(*this, __func__);
  // add the transaction and splits
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmTransactions"].insertString());
  writeTransaction(tx.id(), tx, q, "N");
  ++m_transactions;
  // for each split account, update lastMod date, balance, txCount
  QList<MyMoneySplit>::ConstIterator it_s;
  for(it_s = tx.splits().begin(); it_s != tx.splits().end(); ++it_s) {
    MyMoneyAccount acc = MyMoneyFile::instance()->account((*it_s).accountId());
    ++m_transactionCountMap[acc.id()];
    modifyAccount(acc);
  }
  // in the fileinfo record, update lastMod, txCount, next TxId
  writeFileInfo();
}

void MyMoneyStorageSql::modifyTransaction (const MyMoneyTransaction& tx) {
  DBG("*** Entering MyMoneyStorageSql::modifyTransaction");
  MyMoneyDbTransaction t(*this, __func__);
  // remove the splits of the old tx from the count table
  MyMoneySqlQuery q(this);
  q.prepare ("SELECT accountId FROM kmmSplits WHERE transactionId = :txId;");
  q.bindValue(":txId", tx.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "retrieving old splits"));
  while (q.next()) {
    QString id = q.value(0).toString();
    --m_transactionCountMap[id];
  }
  // add the transaction and splits
  q.prepare (m_db.m_tables["kmmTransactions"].updateString());
  writeTransaction(tx.id(), tx, q, "N");
  // for each split account, update lastMod date, balance, txCount
  QList<MyMoneySplit>::ConstIterator it_s;
  for(it_s = tx.splits().begin(); it_s != tx.splits().end(); ++it_s) {
    //MyMoneyAccount acc = m_storagePtr->account((*it_s).accountId());
    MyMoneyAccount acc = MyMoneyFile::instance()->account((*it_s).accountId());
    ++m_transactionCountMap[acc.id()];
    modifyAccount(acc);
  }
  writeSplits(tx.id(), "N", tx.splits());
  // in the fileinfo record, update lastMod
  writeFileInfo();
}

void MyMoneyStorageSql::removeTransaction(const MyMoneyTransaction& tx) {
  DBG("*** Entering MyMoneyStorageSql::removeTransaction");
  MyMoneyDbTransaction t(*this, __func__);
  deleteTransaction(tx.id());
  --m_transactions;

  // for each split account, update lastMod date, balance, txCount
  QList<MyMoneySplit>::ConstIterator it_s;
  for(it_s = tx.splits().begin(); it_s != tx.splits().end(); ++it_s) {
    MyMoneyAccount acc = m_storagePtr->account((*it_s).accountId());
    --m_transactionCountMap[acc.id()];
    modifyAccount(acc);
  }
  // in the fileinfo record, update lastModDate, txCount
  writeFileInfo();
}

void MyMoneyStorageSql::deleteTransaction(const QString& id) {
  DBG("*** Entering MyMoneyStorageSql::deleteTransaction");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare("DELETE FROM kmmSplits WHERE transactionId = :transactionId;");
  q.bindValue(":transactionId", id);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Splits"));

  q.prepare ("DELETE FROM kmmKeyValuePairs WHERE kvpType = 'SPLIT' "
             "AND kvpId LIKE '" + id + "%'");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Splits KVP"));

  m_splits -= q.numRowsAffected();
  deleteKeyValuePairs("TRANSACTION", id);
  q.prepare(m_db.m_tables["kmmTransactions"].deleteString());
  q.bindValue(":id", id);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Transaction"));
}

void MyMoneyStorageSql::writeTransaction(const QString& txId, const MyMoneyTransaction& tx, MyMoneySqlQuery& q, const QString& type) {
  DBG("*** Entering MyMoneyStorageSql::writeTransaction");
  q.bindValue(":id", txId);
  q.bindValue(":txType", type);
  q.bindValue(":postDate", tx.postDate().toString(Qt::ISODate));
  q.bindValue(":memo", tx.memo());
  q.bindValue(":entryDate", tx.entryDate().toString(Qt::ISODate));
  q.bindValue(":currencyId", tx.commodity());
  q.bindValue(":bankId", tx.bankID());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Transaction")));

  m_txPostDate = tx.postDate(); // FIXME: TEMP till Tom puts date in split object
  QList<MyMoneySplit> splitList = tx.splits();
  writeSplits(txId, type, splitList);

  //Add in Key-Value Pairs for transactions.
  deleteKeyValuePairs("TRANSACTION", txId);
  writeKeyValuePairs("TRANSACTION", txId, tx.pairs());
  m_hiIdTransactions = calcHighId(m_hiIdTransactions, tx.id());
}

void MyMoneyStorageSql::writeSplits(const QString& txId, const QString& type, const QList<MyMoneySplit>& splitList) {
  DBG("*** Entering MyMoneyStorageSql::writeSplits");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<unsigned int> dbList;
  MyMoneySqlQuery q(this);
  q.prepare("SELECT splitId FROM kmmSplits where transactionId = :id;");
  q.bindValue(":id", txId);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Split list"));
  while (q.next()) dbList.append(q.value(0).toUInt());

  QList<MyMoneySplit>::ConstIterator it;
  unsigned int i;
  MyMoneySqlQuery q2(this);
  q.prepare (m_db.m_tables["kmmSplits"].updateString());
  q2.prepare (m_db.m_tables["kmmSplits"].insertString());
  for(it = splitList.begin(), i = 0; it != splitList.end(); ++it, ++i) {
    if (dbList.contains(i)) {
      dbList.removeAll (i);
      writeSplit(txId, (*it), type, i, q);
    } else {
      ++m_splits;
      writeSplit(txId, (*it), type, i, q2);
    }
  }

  if (!dbList.isEmpty()) {
    q.prepare("DELETE FROM kmmSplits WHERE transactionId = :txId AND splitId = :splitId");
    QList<unsigned int>::const_iterator it = dbList.constBegin();
    while (it != dbList.constEnd()) {
      q.bindValue(":txId", txId);
      q.bindValue(":splitId", *it);
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Splits"));
      ++it;
    }
  }
}

void MyMoneyStorageSql::writeSplit(const QString& txId, const MyMoneySplit& split,
                                   const QString& type, const int splitId, MyMoneySqlQuery& q) {
  DBG("*** Entering MyMoneyStorageSql::writeSplit");
  q.bindValue(":transactionId", txId);
  q.bindValue(":txType", type);
  q.bindValue(":splitId", splitId);
  q.bindValue(":payeeId", split.payeeId());
  if (split.reconcileDate() == QDate())
    q.bindValue(":reconcileDate", split.reconcileDate());
  else
    q.bindValue(":reconcileDate", split.reconcileDate().toString(Qt::ISODate));
  q.bindValue(":action", split.action());
  q.bindValue(":reconcileFlag", split.reconcileFlag());
  q.bindValue(":value", split.value().toString());
  q.bindValue(":valueFormatted", split.value()
      .formatMoney("", -1, false)
        .replace(QChar(','), QChar('.')));
  q.bindValue(":shares", split.shares().toString());
  MyMoneyAccount acc = m_storagePtr->account(split.accountId());
  MyMoneySecurity sec = m_storagePtr->security(acc.currencyId());
  q.bindValue(":sharesFormatted",
              split.shares().
                  formatMoney("", MyMoneyMoney::denomToPrec(sec.smallestAccountFraction()), false).
                  replace(QChar(','), QChar('.')));
  MyMoneyMoney price = split.actualPrice();
  if (!price.isZero()) {
    q.bindValue(":price", price.toString());
    q.bindValue(":priceFormatted", price.formatMoney
        ("", KMyMoneySettings::pricePrecision(), false)
            .replace(QChar(','), QChar('.')));
  } else {
    q.bindValue(":price", QString());
    q.bindValue(":priceFormatted", QString());
  }
  q.bindValue(":memo", split.memo());
  q.bindValue(":accountId", split.accountId());
  q.bindValue(":checkNumber", split.number());
  q.bindValue(":postDate", m_txPostDate.toString(Qt::ISODate)); // FIXME: when Tom puts date into split object
  q.bindValue(":bankId", split.bankID());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Split")));
  deleteKeyValuePairs("SPLIT", txId + QString::number(splitId));
  writeKeyValuePairs("SPLIT", txId + QString::number(splitId), split.pairs());
}

// **** Schedules ****
void MyMoneyStorageSql::writeSchedules() {
  DBG("*** Entering MyMoneyStorageSql::writeSchedules");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  q.prepare("SELECT id FROM kmmSchedules;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Schedule list"));
  while (q.next()) dbList.append(q.value(0).toString());

  const QList<MyMoneySchedule> list = m_storage->scheduleList();
  QList<MyMoneySchedule>::ConstIterator it;
  MyMoneySqlQuery q2(this);
  //TODO: find a way to prepare the queries outside of the loop.  writeSchedule()
  // modifies the query passed to it, so they have to be re-prepared every pass.
  signalProgress(0, list.count(), "Writing Schedules...");
  for(it = list.begin(); it != list.end(); ++it) {
    q.prepare (m_db.m_tables["kmmSchedules"].updateString());
    q2.prepare (m_db.m_tables["kmmSchedules"].insertString());
    bool insert = true;
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      insert = false;
      writeSchedule(*it, q, insert);
    } else {
      writeSchedule(*it, q2, insert);
    }
    signalProgress(++m_schedules, 0);
  }

  if (!dbList.isEmpty()) {
    QList<QString>::const_iterator it = dbList.constBegin();
    while (it != dbList.constEnd()) {
      deleteSchedule(*it);
      ++it;
    }
  }
}

void MyMoneyStorageSql::addSchedule(const MyMoneySchedule& sched) {
  DBG("*** Entering MyMoneyStorageSql::addSchedule");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmSchedules"].insertString());
  writeSchedule(sched,q, true);
  ++m_schedules;
  writeFileInfo();
}

void MyMoneyStorageSql::modifySchedule(const MyMoneySchedule& sched) {
  DBG("*** Entering MyMoneyStorageSql::modifySchedule");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmSchedules"].updateString());
  writeSchedule(sched,q, false);
  writeFileInfo();
}

void MyMoneyStorageSql::removeSchedule(const MyMoneySchedule& sched) {
  DBG("*** Entering MyMoneyStorageSql::removeSchedule");
  MyMoneyDbTransaction t(*this, __func__);
  deleteSchedule(sched.id());
  --m_schedules;
  writeFileInfo();
}

void MyMoneyStorageSql::deleteSchedule (const QString& id) {
  DBG("*** Entering MyMoneyStorageSql::deleteSchedule");
  deleteTransaction(id);
  MyMoneySqlQuery q(this);
  q.prepare("DELETE FROM kmmSchedulePaymentHistory WHERE schedId = :id");
  q.bindValue(":id", id);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Schedule Payment History"));
  q.prepare(m_db.m_tables["kmmSchedules"].deleteString());
  q.bindValue(":id", id);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Schedule"));
  //FIXME: enable when schedules have KVPs.
  //deleteKeyValuePairs("SCHEDULE", id);
}

void MyMoneyStorageSql::writeSchedule(const MyMoneySchedule& sch, MyMoneySqlQuery& q, bool insert) {
  DBG("*** Entering MyMoneyStorageSql::writeSchedule");
  q.bindValue(":id", sch.id());
  q.bindValue(":name", sch.name());
  q.bindValue(":type", sch.type());
  q.bindValue(":typeString", MyMoneySchedule::scheduleTypeToString(sch.type()));
  q.bindValue(":occurence", sch.occurencePeriod());
  q.bindValue(":occurenceMultiplier", sch.occurenceMultiplier());
  q.bindValue(":occurenceString", sch.occurenceToString());
  q.bindValue(":paymentType", sch.paymentType());
  q.bindValue(":paymentTypeString", MyMoneySchedule::paymentMethodToString(sch.paymentType()));
  q.bindValue(":startDate", sch.startDate().toString(Qt::ISODate));
  q.bindValue(":endDate", sch.endDate().toString(Qt::ISODate));
  if (sch.isFixed()) {
    q.bindValue(":fixed", "Y");
  } else {
    q.bindValue(":fixed", "N");
  }
  if (sch.autoEnter()) {
    q.bindValue(":autoEnter", "Y");
  } else {
    q.bindValue(":autoEnter", "N");
  }
  q.bindValue(":lastPayment", sch.lastPayment());
  q.bindValue(":nextPaymentDue", sch.nextDueDate().toString(Qt::ISODate));
  q.bindValue(":weekendOption", sch.weekendOption());
  q.bindValue(":weekendOptionString", MyMoneySchedule::weekendOptionToString(sch.weekendOption()));
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Schedules")));

  //store the payment history for this scheduled task.
  //easiest way is to delete all and re-insert; it's not a high use table
  q.prepare("DELETE FROM kmmSchedulePaymentHistory WHERE schedId = :id;");
  q.bindValue(":id", sch.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting  Schedule Payment History")));

  q.prepare (m_db.m_tables["kmmSchedulePaymentHistory"].insertString());
  QList<QDate> payments = sch.recordedPayments();
  QList<QDate>::ConstIterator it;
  for (it=payments.constBegin(); it!=payments.constEnd(); ++it) {
    q.bindValue(":schedId", sch.id());
    q.bindValue(":payDate", (*it).toString(Qt::ISODate));
    if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Schedule Payment History")));
  }

    //store the transaction data for this task.
  if (!insert) {
    q.prepare (m_db.m_tables["kmmTransactions"].updateString());
  } else {
    q.prepare (m_db.m_tables["kmmTransactions"].insertString());
  }
  writeTransaction(sch.id(), sch.transaction(), q, "S");

  //FIXME: enable when schedules have KVPs.

  //Add in Key-Value Pairs for transactions.
  //deleteKeyValuePairs("SCHEDULE", sch.id());
  //writeKeyValuePairs("SCHEDULE", sch.id(), sch.pairs());
  m_hiIdSchedules = calcHighId(m_hiIdSchedules, sch.id());
}

// **** Securities ****
void MyMoneyStorageSql::writeSecurities() {
  DBG("*** Entering MyMoneyStorageSql::writeSecurities");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  MyMoneySqlQuery q2(this);
  q.prepare("SELECT id FROM kmmSecurities;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building security list"));
  while (q.next()) dbList.append(q.value(0).toString());

  const QList<MyMoneySecurity> securityList = m_storage->securityList();
  signalProgress(0, securityList.count(), "Writing Securities...");
  q.prepare (m_db.m_tables["kmmSecurities"].updateString());
  q2.prepare (m_db.m_tables["kmmSecurities"].insertString());
  for(QList<MyMoneySecurity>::ConstIterator it = securityList.begin(); it != securityList.end(); ++it) {
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      writeSecurity((*it), q);
    } else {
      writeSecurity((*it), q2);
    }
    signalProgress(++m_securities, 0);
  }

  if (!dbList.isEmpty()) {
    q.prepare("DELETE FROM kmmSecurities WHERE id = :id");
    q2.prepare("DELETE FROM kmmPrices WHERE fromId = :id OR toId = :id");
    QList<QString>::const_iterator it = dbList.constBegin();
    while (it != dbList.constEnd()) {
      q.bindValue(":id", (*it));
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Security"));
      q2.bindValue(":fromId", (*it));
      q2.bindValue(":toId", (*it));
      if (!q2.exec()) throw new MYMONEYEXCEPTION(buildError (q2, __func__, "deleting Security"));
      deleteKeyValuePairs("SECURITY", (*it));
       ++it;
    }
  }
}

void MyMoneyStorageSql::addSecurity(const MyMoneySecurity& sec) {
  DBG("*** Entering MyMoneyStorageSql::addSecurity");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmSecurities"].insertString());
  writeSecurity(sec,q);
  ++m_securities;
  writeFileInfo();
}

void MyMoneyStorageSql::modifySecurity(const MyMoneySecurity& sec) {
  DBG("*** Entering MyMoneyStorageSql::modifySecurity");
  MyMoneyDbTransaction t(*this, __func__);
  deleteKeyValuePairs("SECURITY", sec.id());
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmSecurities"].updateString());
  writeSecurity(sec,q);
  writeFileInfo();
}

void MyMoneyStorageSql::removeSecurity(const MyMoneySecurity& sec) {
  DBG("*** Entering MyMoneyStorageSql::removeSecurity");
  MyMoneyDbTransaction t(*this, __func__);
  deleteKeyValuePairs("SECURITY", sec.id());
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmSecurities"].deleteString());
  q.bindValue(":id", sec.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting Security")));
  --m_securities;
  writeFileInfo();
}

void MyMoneyStorageSql::writeSecurity(const MyMoneySecurity& security, MyMoneySqlQuery& q) {
  DBG("*** Entering MyMoneyStorageSql::writeSecurity");
  q.bindValue(":id", security.id());
  q.bindValue(":name", security.name());
  q.bindValue(":symbol", security.tradingSymbol());
  q.bindValue(":type", static_cast<int>(security.securityType()));
  q.bindValue(":typeString", MyMoneySecurity::securityTypeToString(security.securityType()));
  q.bindValue(":smallestAccountFraction", security.smallestAccountFraction());
  q.bindValue(":tradingCurrency", security.tradingCurrency());
  q.bindValue(":tradingMarket", security.tradingMarket());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString ("writing Securities")));

  //Add in Key-Value Pairs for security
  writeKeyValuePairs("SECURITY", security.id(), security.pairs());
  m_hiIdSecurities = calcHighId(m_hiIdSecurities, security.id());
}

// **** Prices ****
void MyMoneyStorageSql::writePrices() {
  DBG("*** Entering MyMoneyStorageSql::writePrices");
  // due to difficulties in matching and determining deletes
  // easiest way is to delete all and re-insert
  MyMoneySqlQuery q(this);
  q.prepare("DELETE FROM kmmPrices");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting Prices")));
  m_prices = 0;

  const MyMoneyPriceList list = m_storage->priceList();
  signalProgress(0, list.count(), "Writing Prices...");
  MyMoneyPriceList::ConstIterator it;
  for(it = list.begin(); it != list.end(); ++it)   {
    writePricePair(*it);
  }
}

void MyMoneyStorageSql::writePricePair(const MyMoneyPriceEntries& p) {
  DBG("*** Entering MyMoneyStorageSql::writePricePair");
  MyMoneyPriceEntries::ConstIterator it;
  for(it = p.begin(); it != p.end(); ++it) {
    writePrice (*it);
    signalProgress(++m_prices, 0);
  }
}

void MyMoneyStorageSql::addPrice(const MyMoneyPrice& p) {
  DBG("*** Entering MyMoneyStorageSql::addPrice");
  if (m_readingPrices) return;
  // the app always calls addPrice, whether or not there is already one there
  MyMoneyDbTransaction t(*this, __func__);
  bool newRecord = false;
  MyMoneySqlQuery q(this);
  QString s = m_db.m_tables["kmmPrices"].selectAllString(false);
  s += " WHERE fromId = :fromId AND toId = :toId AND priceDate = :priceDate;";
  q.prepare (s);
  q.bindValue(":fromId", p.from());
  q.bindValue(":toId", p.to());
  q.bindValue(":priceDate", p.date().toString(Qt::ISODate));
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("finding Price")));
  if (q.next()) {
    q.prepare(m_db.m_tables["kmmPrices"].updateString());
  } else {
    q.prepare(m_db.m_tables["kmmPrices"].insertString());
    ++m_prices;
    newRecord = true;
  }
  q.bindValue(":fromId", p.from());
  q.bindValue(":toId", p.to());
  q.bindValue(":priceDate", p.date().toString(Qt::ISODate));
  q.bindValue(":price", p.rate(QString()).toString());
  q.bindValue(":priceFormatted",
    p.rate(QString()).formatMoney("", KMyMoneySettings::pricePrecision()));
  q.bindValue(":priceSource", p.source());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Price")));

  if (newRecord) writeFileInfo();
}

void MyMoneyStorageSql::removePrice(const MyMoneyPrice& p) {
  DBG("*** Entering MyMoneyStorageSql::removePrice");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmPrices"].deleteString());
  q.bindValue(":fromId", p.from());
  q.bindValue(":toId", p.to());
  q.bindValue(":priceDate", p.date().toString(Qt::ISODate));
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting Price")));
  --m_prices;
  writeFileInfo();
}

void MyMoneyStorageSql::writePrice(const MyMoneyPrice& p) {
  DBG("*** Entering MyMoneyStorageSql::writePrice");
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmPrices"].insertString());
  q.bindValue(":fromId", p.from());
  q.bindValue(":toId", p.to());
  q.bindValue(":priceDate", p.date().toString(Qt::ISODate));
  q.bindValue(":price", p.rate(QString()).toString());
  q.bindValue(":priceFormatted", p.rate(QString()).formatMoney("", 2));
  q.bindValue(":priceSource", p.source());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Prices")));
}

// **** Currencies ****
void MyMoneyStorageSql::writeCurrencies() {
  DBG("*** Entering MyMoneyStorageSql::writeCurrencies");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  MyMoneySqlQuery q2(this);
  q.prepare("SELECT ISOCode FROM kmmCurrencies;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Currency list"));
  while (q.next()) dbList.append(q.value(0).toString());

  const QList<MyMoneySecurity> currencyList = m_storage->currencyList();
  signalProgress(0, currencyList.count(), "Writing Currencies...");
  q.prepare (m_db.m_tables["kmmCurrencies"].updateString());
  q2.prepare (m_db.m_tables["kmmCurrencies"].insertString());
  for(QList<MyMoneySecurity>::ConstIterator it = currencyList.begin(); it != currencyList.end(); ++it) {
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      writeCurrency((*it), q);
    } else {
      writeCurrency((*it), q2);
    }
    signalProgress(++m_currencies, 0);
  }

  if (!dbList.isEmpty()) {
    q.prepare("DELETE FROM kmmCurrencies WHERE ISOCode = :ISOCode");
    QList<QString>::const_iterator it = dbList.constBegin();
    while (it != dbList.constEnd()) {
      q.bindValue(":ISOCode", (*it));
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Currency"));
      ++it;
    }
  }
}

void MyMoneyStorageSql::addCurrency(const MyMoneySecurity& sec) {
  DBG("*** Entering MyMoneyStorageSql::addCurrency");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmCurrencies"].insertString());
  writeCurrency(sec,q);
  ++m_currencies;
  writeFileInfo();
}

void MyMoneyStorageSql::modifyCurrency(const MyMoneySecurity& sec) {
  DBG("*** Entering MyMoneyStorageSql::modifyCurrency");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmCurrencies"].updateString());
  writeCurrency(sec,q);
  writeFileInfo();
}

void MyMoneyStorageSql::removeCurrency(const MyMoneySecurity& sec) {
  DBG("*** Entering MyMoneyStorageSql::removeCurrency");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmCurrencies"].deleteString());
  q.bindValue(":ISOcode", sec.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting Currency")));
  --m_currencies;
  writeFileInfo();
}

void MyMoneyStorageSql::writeCurrency(const MyMoneySecurity& currency, MyMoneySqlQuery& q) {
  DBG("*** Entering MyMoneyStorageSql::writeCurrency");
  q.bindValue(":ISOcode", currency.id());
  q.bindValue(":name", currency.name());
  q.bindValue(":type", static_cast<int>(currency.securityType()));
  q.bindValue(":typeString", MyMoneySecurity::securityTypeToString(currency.securityType()));
  // writing the symbol as three short ints is a PITA, but the
  // problem is that database drivers have incompatible ways of declaring UTF8
  QString symbol = currency.tradingSymbol() + "   ";
  q.bindValue(":symbol1", symbol.mid(0,1).unicode()->unicode());
  q.bindValue(":symbol2", symbol.mid(1,1).unicode()->unicode());
  q.bindValue(":symbol3", symbol.mid(2,1).unicode()->unicode());
  q.bindValue(":symbolString", symbol);
  q.bindValue(":partsPerUnit", currency.partsPerUnit());
  q.bindValue(":smallestCashFraction", currency.smallestCashFraction());
  q.bindValue(":smallestAccountFraction", currency.smallestAccountFraction());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Currencies")));
}


void MyMoneyStorageSql::writeReports() {
  DBG("*** Entering MyMoneyStorageSql::writeReports");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  MyMoneySqlQuery q2(this);
  q.prepare("SELECT id FROM kmmReportConfig;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Report list"));
  while (q.next()) dbList.append(q.value(0).toString());

  QList<MyMoneyReport> list = m_storage->reportList();
  signalProgress(0, list.count(), "Writing Reports...");
  QList<MyMoneyReport>::ConstIterator it;
  q.prepare (m_db.m_tables["kmmReportConfig"].updateString());
  q2.prepare (m_db.m_tables["kmmReportConfig"].insertString());
  for(it = list.constBegin(); it != list.constEnd(); ++it){
    if (dbList.contains((*it).id())) {
      dbList.removeAll ((*it).id());
      writeReport(*it, q);
    } else {
      writeReport(*it, q2);
    }
    signalProgress(++m_reports, 0);
  }

  if (!dbList.isEmpty()) {
    q.prepare("DELETE FROM kmmReportConfig WHERE id = :id");
    QList<QString>::const_iterator it = dbList.constBegin();
    while (it != dbList.constEnd()) {
      q.bindValue(":id", (*it));
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Report"));
      ++it;
    }
  }
}

void MyMoneyStorageSql::addReport(const MyMoneyReport& rep) {
  DBG("*** Entering MyMoneyStorageSql::addReport");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmReportConfig"].insertString());
  writeReport(rep,q);
  ++m_reports;
  writeFileInfo();
}

void MyMoneyStorageSql::modifyReport(const MyMoneyReport& rep) {
  DBG("*** Entering MyMoneyStorageSql::modifyReport");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmReportConfig"].updateString());
  writeReport(rep,q);
  writeFileInfo();
}

void MyMoneyStorageSql::removeReport(const MyMoneyReport& rep) {
  DBG("*** Entering MyMoneyStorageSql::removeReport");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare("DELETE FROM kmmReportConfig WHERE id = :id");
  q.bindValue(":id", rep.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting Report")));
  --m_reports;
  writeFileInfo();
}

void MyMoneyStorageSql::writeReport (const MyMoneyReport& rep, MyMoneySqlQuery& q) {
  DBG("*** Entering MyMoneyStorageSql::writeReport");
  QDomDocument d; // create a dummy XML document
  QDomElement e = d.createElement("REPORTS");
  d.appendChild (e);
  rep.writeXML(d, e); // write the XML to document
  q.bindValue(":id", rep.id());
  q.bindValue(":name", rep.name());
  q.bindValue(":XML", d.toString());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Reports")));
}

void MyMoneyStorageSql::writeBudgets() {
  DBG("*** Entering MyMoneyStorageSql::writeBudgets");
  // first, get a list of what's on the database (see writeInstitutions)
  QList<QString> dbList;
  MyMoneySqlQuery q(this);
  MyMoneySqlQuery q2(this);
  q.prepare("SELECT name FROM kmmBudgetConfig;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "building Budget list"));
  while (q.next()) dbList.append(q.value(0).toString());

  QList<MyMoneyBudget> list = m_storage->budgetList();
  signalProgress(0, list.count(), "Writing Budgets...");
  QList<MyMoneyBudget>::ConstIterator it;
  q.prepare (m_db.m_tables["kmmBudgetConfig"].updateString());
  q2.prepare (m_db.m_tables["kmmBudgetConfig"].insertString());
  for(it = list.constBegin(); it != list.constEnd(); ++it){
    if (dbList.contains((*it).name())) {
      dbList.removeAll ((*it).name());
      writeBudget(*it, q);
    } else {
      writeBudget(*it, q2);
    }
    signalProgress(++m_budgets, 0);
  }

  if (!dbList.isEmpty()) {
    q.prepare("DELETE FROM kmmBudgetConfig WHERE id = :id");
    QList<QString>::const_iterator it = dbList.constBegin();
    while (it != dbList.constEnd()) {
      q.bindValue(":name", (*it));
      if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "deleting Budget"));
      ++it;
    }
  }
}

void MyMoneyStorageSql::addBudget(const MyMoneyBudget& bud) {
  DBG("*** Entering MyMoneyStorageSql::addBudget");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmBudgetConfig"].insertString());
  writeBudget(bud,q);
  ++m_budgets;
  writeFileInfo();
}

void MyMoneyStorageSql::modifyBudget(const MyMoneyBudget& bud) {
  DBG("*** Entering MyMoneyStorageSql::modifyBudget");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmBudgetConfig"].updateString());
  writeBudget(bud,q);
  writeFileInfo();
}

void MyMoneyStorageSql::removeBudget(const MyMoneyBudget& bud) {
  DBG("*** Entering MyMoneyStorageSql::removeBudget");
  MyMoneyDbTransaction t(*this, __func__);
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmBudgetConfig"].deleteString());
  q.bindValue(":id", bud.id());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting Budget")));
  --m_budgets;
  writeFileInfo();
}

void MyMoneyStorageSql::writeBudget (const MyMoneyBudget& bud, MyMoneySqlQuery& q) {
  DBG("*** Entering MyMoneyStorageSql::writeBudget");
  QDomDocument d; // create a dummy XML document
  QDomElement e = d.createElement("BUDGETS");
  d.appendChild (e);
  bud.writeXML(d, e); // write the XML to document
  q.bindValue(":id", bud.id());
  q.bindValue(":name", bud.name());
  q.bindValue(":start", bud.budgetStart());
  q.bindValue(":XML", d.toString());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing Budgets")));
}

void MyMoneyStorageSql::writeFileInfo() {
  DBG("*** Entering MyMoneyStorageSql::writeFileInfo");
  // we have no real way of knowing when these change, so re-write them every time
  deleteKeyValuePairs("STORAGE", "");
  writeKeyValuePairs("STORAGE", "", m_storage->pairs());
  //
  MyMoneySqlQuery q(this);
  q.prepare ("SELECT * FROM kmmFileInfo;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, "checking fileinfo"));
  QString qs;
  if (q.next())
    qs = m_db.m_tables["kmmFileInfo"].updateString();
  else
    qs = (m_db.m_tables["kmmFileInfo"].insertString());
  q.prepare(qs);
  q.bindValue(":version", m_dbVersion);
  q.bindValue(":fixLevel", m_storage->fileFixVersion());
  q.bindValue(":created", m_storage->creationDate().toString(Qt::ISODate));
  //q.bindValue(":lastModified", m_storage->lastModificationDate().toString(Qt::ISODate));
  q.bindValue(":lastModified", QDate::currentDate().toString(Qt::ISODate));
  q.bindValue(":baseCurrency", m_storage->pairs()["kmm-baseCurrency"]);
  q.bindValue(":institutions", (unsigned long long) m_institutions);
  q.bindValue(":accounts", (unsigned long long) m_accounts);
  q.bindValue(":payees", (unsigned long long) m_payees);
  q.bindValue(":transactions", (unsigned long long) m_transactions);
  q.bindValue(":splits", (unsigned long long) m_splits);
  q.bindValue(":securities", (unsigned long long) m_securities);
  q.bindValue(":prices", (unsigned long long) m_prices);
  q.bindValue(":currencies", (unsigned long long) m_currencies);
  q.bindValue(":schedules", (unsigned long long) m_schedules);
  q.bindValue(":reports", (unsigned long long) m_reports);
  q.bindValue(":kvps", (unsigned long long) m_kvps);
  q.bindValue(":budgets", (unsigned long long) m_budgets);
  q.bindValue(":dateRangeStart", QDate());
  q.bindValue(":dateRangeEnd", QDate());

  //FIXME: This modifies all m_<variable> used in this function.
  // Sometimes the memory has been updated.

  // Should most of these be tracked in a view?
  // Variables actually needed are: version, fileFixVersion, creationDate,
  // baseCurrency, encryption, update info, and logon info.
  //try {
    //readFileInfo();
  //} catch (...) {
    //startCommitUnit(__func__);
  //}

  q.bindValue(":hiInstitutionId", (unsigned long long) m_hiIdInstitutions);
  q.bindValue(":hiPayeeId", (unsigned long long) m_hiIdPayees);
  q.bindValue(":hiAccountId", (unsigned long long) m_hiIdAccounts);
  q.bindValue(":hiTransactionId", (unsigned long long) m_hiIdTransactions);
  q.bindValue(":hiScheduleId", (unsigned long long) m_hiIdSchedules);
  q.bindValue(":hiSecurityId", (unsigned long long) m_hiIdSecurities);
  q.bindValue(":hiReportId", (unsigned long long) m_hiIdReports);
  q.bindValue(":hiBudgetId", (unsigned long long) m_hiIdBudgets);

  q.bindValue(":encryptData", m_encryptData);
  q.bindValue(":updateInProgress", "N");
  q.bindValue(":logonUser", m_logonUser);
  q.bindValue(":logonAt", m_logonAt.toString(Qt::ISODate));
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing FileInfo")));
}

// **** Key/value pairs ****
void MyMoneyStorageSql::writeKeyValuePairs(const QString& kvpType, const QString& kvpId, const QMap<QString,  QString>& pairs) {
  DBG("*** Entering MyMoneyStorageSql::writeKeyValuePairs");
  QMap<QString, QString>::ConstIterator it;
  for(it = pairs.begin(); it != pairs.end(); ++it) {
    writeKeyValuePair (kvpType, kvpId, it.key(), it.value());
  }
}

void MyMoneyStorageSql::writeKeyValuePair (const QString& kvpType, const QString& kvpId, const QString& kvpKey, const QString& kvpData) {
  DBG("*** Entering MyMoneyStorageSql::writeKeyValuePair");
  MyMoneySqlQuery q(this);
  q.prepare (m_db.m_tables["kmmKeyValuePairs"].insertString());
  q.bindValue(":kvpType", kvpType);
  q.bindValue(":kvpId", kvpId);
  q.bindValue(":kvpKey", kvpKey);
  q.bindValue(":kvpData", kvpData);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("writing KVP")));
  ++m_kvps;
}

void MyMoneyStorageSql::deleteKeyValuePairs (const QString& kvpType, const QString& kvpId) {
  DBG("*** Entering MyMoneyStorageSql::deleteKeyValuePairs");
  MyMoneySqlQuery q(this);
  q.prepare ("DELETE FROM kmmKeyValuePairs WHERE kvpType = :kvpType AND kvpId = :kvpId;");
  q.bindValue(":kvpType", kvpType);
  q.bindValue(":kvpId", kvpId);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("deleting kvp for %1 %2").arg(kvpType).arg(kvpId)));
  m_kvps -= q.numRowsAffected();
}

//******************************** read SQL routines **************************************
#define CASE(a) if ((*ft)->name() == #a)
#define GETSTRING q.value(i).toString()
#define GETDATE getDate(GETSTRING)
#define GETDATETIME getDateTime(GETSTRING)
#define GETINT q.value(i).toInt()
#define GETULL q.value(i).toULongLong()

void MyMoneyStorageSql::readFileInfo(void) {
  DBG("*** Entering MyMoneyStorageSql::readFileInfo");
  signalProgress(0, 18, QObject::tr("Loading file information..."));
  MyMoneyDbTable& t = m_db.m_tables["kmmFileInfo"];
  MyMoneySqlQuery q(this);
  q.prepare (t.selectAllString());
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading FileInfo")));
  if (!q.next()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("retrieving FileInfo")));
  MyMoneyDbTable::field_iterator ft = t.begin();
  int i = 0;
  while (ft != t.end()) {
    // versioning is now handled in open routine
/*    CASE(version)  setVersion(GETSTRING); // check version == current version...
    else*/
    CASE(created) m_storage->setCreationDate(GETDATE);
    else CASE(lastModified) m_storage->setLastModificationDate(GETDATE);
    else CASE(hiInstitutionId) m_hiIdInstitutions = (unsigned long) GETULL;
    else CASE(hiPayeeId)       m_hiIdPayees = (unsigned long) GETULL;
    else CASE(hiAccountId)     m_hiIdAccounts = (unsigned long) GETULL;
    else CASE(hiTransactionId) m_hiIdTransactions = (unsigned long) GETULL;
    else CASE(hiScheduleId)    m_hiIdSchedules = (unsigned long) GETULL;
    else CASE(hiSecurityId)    m_hiIdSecurities = (unsigned long) GETULL;
    else CASE(hiReportId  )    m_hiIdReports = (unsigned long) GETULL;
    else CASE(hiBudgetId  )    m_hiIdBudgets = (unsigned long) GETULL;
    else CASE(institutions) m_institutions = (unsigned long) GETULL;
    else CASE(accounts    ) m_accounts = (unsigned long) GETULL;
    else CASE(payees      ) m_payees = (unsigned long) GETULL;
    else CASE(transactions) m_transactions = (unsigned long) GETULL;
    else CASE(splits      ) m_splits = (unsigned long) GETULL;
    else CASE(securities  ) m_securities = (unsigned long) GETULL;
    else CASE(currencies  ) m_currencies = (unsigned long) GETULL;
    else CASE(schedules   ) m_schedules = (unsigned long) GETULL;
    else CASE(prices      ) m_prices = (unsigned long) GETULL;
    else CASE(kvps        ) m_kvps = (unsigned long) GETULL;
    else CASE(reports     ) m_reports = (unsigned long) GETULL;
    else CASE(budgets     ) m_budgets = (unsigned long) GETULL;
    else CASE(encryptData) m_encryptData = GETSTRING;
    else CASE(logonUser)  m_logonUser = GETSTRING;
    else CASE(logonAt)  m_logonAt = GETDATETIME;
    ++ft; ++i;
    signalProgress(i,0);
  }
  m_storage->setPairs(readKeyValuePairs("STORAGE", QString("")).pairs());
}

/*void MyMoneyStorageSql::setVersion (const QString& version) {
  DBG("*** Entering MyMoneyStorageSql::setVersion");
  m_dbVersion = version.section('.', 0, 0).toUInt();
  m_minorVersion = version.section('.', 1, 1).toUInt();
  // Okay, I made a cockup by forgetting to include a fixversion in the database
  // design, so we'll use the minor version as fix level (similar to VERSION
  // and FIXVERSION in XML file format). A second mistake was setting minor version to 1
  // in the first place, so we need to subtract one on reading and add one on writing (sigh)!!
  m_storage->setFileFixVersion( m_minorVersion - 1);
}*/

void MyMoneyStorageSql::readInstitutions(void) {
  TRY
  QMap<QString, MyMoneyInstitution> iList = fetchInstitutions();
  m_storage->loadInstitutions(iList);
  readFileInfo();
  m_storage->loadInstitutionId(m_hiIdInstitutions);
  PASS
}

const QMap<QString, MyMoneyInstitution> MyMoneyStorageSql::fetchInstitutions (const QStringList& idList, bool forUpdate) const {
  DBG("*** Entering MyMoneyStorageSql::fetchInstitutions");
  signalProgress(0, m_institutions, QObject::tr("Loading institutions..."));
  int progress = 0;
  QMap<QString, MyMoneyInstitution> iList;
  unsigned long lastId = 0;
  const MyMoneyDbTable& t = m_db.m_tables["kmmInstitutions"];
  MyMoneySqlQuery sq(const_cast <MyMoneyStorageSql*> (this));
  sq.prepare ("SELECT id from kmmAccounts where institutionId = :id");
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  QString queryString (t.selectAllString(false));

  // Use bind variables, instead of just inserting the values in the queryString,
  // so that values containing a ':' will work.
  if (! idList.empty()) {
    queryString += " WHERE";
    for (int i = 0; i < idList.count(); ++i)
      queryString += " id = :id" + QString::number(i) + " OR";
    queryString = queryString.left(queryString.length() - 2);
  }
  if (forUpdate)
    queryString += " FOR UPDATE";

  queryString += ";";

  q.prepare (queryString);

  if (! idList.empty()) {
    QStringList::ConstIterator bindVal = idList.begin();
    for (int i = 0; bindVal != idList.end(); ++i, ++bindVal) {
      q.bindValue (":id" + QString::number(i), *bindVal);
    }
  }

  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Institution")));
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QString iid;
    MyMoneyInstitution inst;
    while (ft != t.end()) {
      CASE(id) iid = GETSTRING;
      else CASE(name) inst.setName(GETSTRING);
      else CASE(manager) inst.setManager(GETSTRING);
      else CASE(routingCode) inst.setSortcode(GETSTRING);
      else CASE(addressStreet) inst.setStreet(GETSTRING);
      else CASE(addressCity) inst.setCity(GETSTRING);
      else CASE(addressZipcode) inst.setPostcode(GETSTRING);
      else CASE(telephone)  inst.setTelephone(GETSTRING);
      ++ft; ++i;
    }
    // get list of subaccounts
    sq.bindValue(":id", iid);
    if (!sq.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Institution AccountList")));
    QStringList aList;
    while (sq.next()) aList.append(sq.value(0).toString());
    for (QStringList::ConstIterator it = aList.constBegin(); it != aList.constEnd(); ++it)
      inst.addAccountId(*it);

    iList[iid] = MyMoneyInstitution(iid, inst);
    unsigned long id = extractId(iid);
    if(id > lastId)
      lastId = id;

    signalProgress (++progress, 0);
  }
  return iList;
}

void MyMoneyStorageSql::readPayees (const QString& id) {
  DBG("*** Entering MyMoneyStorageSql::readPayees(single id)");
  QList<QString> list;
  list.append(id);
  readPayees(list);
}

void MyMoneyStorageSql::readPayees(const QList<QString> pid) {
  DBG("*** Entering MyMoneyStorageSql::readPayees(list)");
  TRY
  //QStringList pidList(pid);
  //qCopy(pid.begin(), pid.end(), pidList.begin()); // aborted here with qt4

  m_storage->loadPayees(fetchPayees(pid));
  readFileInfo();
  m_storage->loadPayeeId(m_hiIdPayees);
  CATCH
    delete e; // ignore duplicates
  ECATCH
//  if (pid.isEmpty()) m_payeeListRead = true;
}

const QMap<QString, MyMoneyPayee> MyMoneyStorageSql::fetchPayees (const QStringList& idList, bool /*forUpdate*/) const {
  DBG("*** Entering MyMoneyStorageSql::fetchPayees");
  MyMoneyDbTransaction trans(const_cast <MyMoneyStorageSql&>(*this), __func__);
  if (m_displayStatus) {
    signalProgress(0, m_payees, QObject::tr("Loading payees..."));
  } else {
//    if (m_payeeListRead) return;
  }
  int progress = 0;
  QMap<QString, MyMoneyPayee> pList;
  //unsigned long lastId;
  const MyMoneyDbTable& t = m_db.m_tables["kmmPayees"];
  MyMoneyDbTable::field_iterator payeeEnd = t.end();
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  if (idList.isEmpty()) {
    q.prepare (t.selectAllString());
  } else {
    QString whereClause = " where (";
    QString itemConnector = "";
    QStringList::ConstIterator it;
    for (it = idList.begin(); it != idList.end(); ++it) {
      whereClause.append(QString("%1id = '%2'").arg(itemConnector).arg(*it));
      itemConnector = " or ";
    }
    whereClause += ")";
    q.prepare (t.selectAllString(false) + whereClause);
  }
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Payee")));
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QString pid;
    QString boolChar;
    MyMoneyPayee payee;
    unsigned int type;
    bool ignoreCase;
    QString matchKeys;
    while (ft != payeeEnd) {
      CASE(id) pid = GETSTRING;
      else CASE(name) payee.setName(GETSTRING);
      else CASE(reference) payee.setReference(GETSTRING);
      else CASE(email) payee.setEmail(GETSTRING);
      else CASE(addressStreet) payee.setAddress(GETSTRING);
      else CASE(addressCity) payee.setCity(GETSTRING);
      else CASE(addressZipcode) payee.setPostcode(GETSTRING);
      else CASE(addressState) payee.setState(GETSTRING);
      else CASE(telephone) payee.setTelephone(GETSTRING);
      else CASE(notes) payee.setNotes(GETSTRING);
      else CASE(defaultAccountId) payee.setDefaultAccountId(GETSTRING);
      else CASE(matchData) type = GETINT;
      else CASE(matchIgnoreCase) ignoreCase = (GETSTRING == "Y");
      else CASE(matchKeys) matchKeys = GETSTRING;
      ++ft; ++i;
    }
    payee.setMatchData (static_cast<MyMoneyPayee::payeeMatchType>(type), ignoreCase, matchKeys);
    if (pid == "USER") {
      TRY
      m_storage->setUser(payee);
      PASS
    } else {
      pList[pid] = MyMoneyPayee(pid, payee);
      //unsigned long id = extractId(QString(pid));
      //if(id > lastId)
      //  lastId = id;
    }
    if (m_displayStatus) signalProgress(++progress, 0);
  }
  return pList;
}

const QMap<QString, MyMoneyAccount> MyMoneyStorageSql::fetchAccounts (const QStringList& idList, bool forUpdate) const {
  DBG("*** Entering MyMoneyStorageSql::fetchAccounts");
  signalProgress(0, m_accounts, QObject::tr("Loading accounts..."));
  int progress = 0;
  QMap<QString, MyMoneyAccount> accList;
  QStringList kvpAccountList;

  const MyMoneyDbTable& t = m_db.m_tables["kmmAccounts"];
  MyMoneyDbTable::field_iterator accEnd = t.end();
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  MyMoneySqlQuery sq(const_cast <MyMoneyStorageSql*> (this));

  QString childQueryString = "SELECT id, parentId FROM kmmAccounts WHERE ";
  QString queryString (t.selectAllString(false));

  // Use bind variables, instead of just inserting the values in the queryString,
  // so that values containing a ':' will work.
  if (! idList.empty()) {
    kvpAccountList = idList;
    queryString += " WHERE id IN (";
    childQueryString += " parentId IN (";
    for (int i = 0; i < idList.count(); ++i) {
      queryString += " :id" + QString::number(i) + ", ";
      childQueryString += ":id" + QString::number(i) + ", ";
    }
    queryString = queryString.left(queryString.length() - 2) + ")";
    childQueryString = childQueryString.left(childQueryString.length() - 2) + ")";
  } else {
    childQueryString += " NOT parentId IS NULL";
  }

  queryString += " ORDER BY id";
  childQueryString += " ORDER BY parentid, id";

  if (forUpdate) {
    queryString += " FOR UPDATE";
    childQueryString += " FOR UPDATE";
  }

  q.prepare (queryString);
  sq.prepare (childQueryString);

  if (! idList.empty()) {
    QStringList::ConstIterator bindVal = idList.begin();
    for (int i = 0; bindVal != idList.end(); ++i, ++bindVal) {
      q.bindValue (":id" + QString::number(i), *bindVal);
      sq.bindValue (":id" + QString::number(i), *bindVal);
    }
  }

  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Account")));
  if (!sq.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading subAccountList")));
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QString aid;
    QString balance;
    MyMoneyAccount acc;

    while (ft != accEnd) {
      CASE(id) aid = GETSTRING;
      else CASE(institutionId) acc.setInstitutionId(GETSTRING);
      else CASE(parentId) acc.setParentAccountId(GETSTRING);
      else CASE(lastReconciled) acc.setLastReconciliationDate(GETDATE);
      else CASE(lastModified) acc.setLastModified(GETDATE);
      else CASE(openingDate) acc.setOpeningDate(GETDATE);
      else CASE(accountNumber) acc.setNumber(GETSTRING);
      else CASE(accountType) acc.setAccountType(static_cast<MyMoneyAccount::accountTypeE>(GETINT));
      else CASE(accountName) acc.setName(GETSTRING);
      else CASE(description) acc.setDescription(GETSTRING);
      else CASE(currencyId) acc.setCurrencyId(GETSTRING);
      else CASE(balance) acc.setBalance(GETSTRING);
      else CASE(transactionCount)
        const_cast <MyMoneyStorageSql*> (this)->m_transactionCountMap[aid] = (unsigned long) GETULL;
      ++ft; ++i;
    }

    // Process any key value pair
    if (idList.empty())
      kvpAccountList.append(aid);

    // in database mode, load the balance from the account record
    // else we would need to read all the transactions
    accList.insert(aid, MyMoneyAccount(aid, acc));
    if (acc.value("PreferredAccount") == "Yes") {
      const_cast <MyMoneyStorageSql*> (this)->m_preferred.addAccount(aid);
    }
    signalProgress(++progress, 0);
  }

    QMap<QString, MyMoneyAccount>::Iterator it_acc;
    QMap<QString, MyMoneyAccount>::Iterator accListEnd = accList.end();
    while (sq.next()) {
      it_acc = accList.find(sq.value(1).toString());
      if (it_acc != accListEnd && it_acc.value().id() == sq.value(1).toString()) {
        while (sq.isValid() && it_acc != accListEnd
            && it_acc.value().id() == sq.value(1).toString()) {
          it_acc.value().addAccountId(sq.value(0).toString());
          sq.next();
        }
        sq.previous();
      }
    }

  //TODO: There should be a better way than this.  What's below is O(n log n) or more,
  // where it may be able to be done in O(n), if things are just right.
  // The operator[] call in the loop is the most expensive call in this function, according
  // to several profile runs.
  QMap <QString, MyMoneyKeyValueContainer> kvpResult = readKeyValuePairs("ACCOUNT", kvpAccountList);
  QMap <QString, MyMoneyKeyValueContainer>::const_iterator kvp_end = kvpResult.constEnd();
  for (QMap <QString, MyMoneyKeyValueContainer>::const_iterator it_kvp = kvpResult.constBegin();
         it_kvp != kvp_end; ++it_kvp) {
    accList[it_kvp.key()].setPairs(it_kvp.value().pairs());
  }

  kvpResult = readKeyValuePairs("ONLINEBANKING", kvpAccountList);
  kvp_end = kvpResult.constEnd();
  for (QMap <QString, MyMoneyKeyValueContainer>::const_iterator it_kvp = kvpResult.constBegin();
         it_kvp != kvp_end; ++it_kvp) {
    accList[it_kvp.key()].setOnlineBankingSettings(it_kvp.value());
  }

  return accList;
}

void MyMoneyStorageSql::readAccounts(void) {
  m_storage->loadAccounts(fetchAccounts());
  m_storage->loadAccountId(m_hiIdAccounts);
}

const QMap<QString, MyMoneyMoney> MyMoneyStorageSql::fetchBalance(const QStringList& idList, const QDate& date) const {

  QMap<QString, MyMoneyMoney> returnValue;
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  QString queryString = "SELECT action, shares, accountId, postDate "
                        "FROM kmmSplits WHERE txType = 'N' AND accountId in (";

  for (int i = 0; i < idList.count(); ++i) {
    queryString += " :id" + QString::number(i) + ", ";
  }
  queryString = queryString.left(queryString.length() - 2) + " )";

  // SQLite stores dates as YYYY-MM-DDTHH:mm:ss with 0s for the time part. This makes
  // the <= operator misbehave when the date matches. To avoid this, add a day to the
  // requested date and use the < operator.
  if (date.isValid() && !date.isNull())
    queryString += QString(" AND postDate < '%1'").arg(date.addDays(1).toString(Qt::ISODate));
  DBG (queryString);
  q.prepare(queryString);

  QStringList::ConstIterator bindVal = idList.begin();
  for (int i = 0; bindVal != idList.end(); ++i, ++bindVal) {
    q.bindValue (":id" + QString::number(i), *bindVal);
    returnValue[*bindVal] = MyMoneyMoney(0);
  }
  if (!q.exec())
    throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("fetching balance")));
  QString id;
  QString shares;
  QString action;
  while (q.next()) {
    id = q.value(2).toString();
    shares = q.value(1).toString();
    action = q.value(0).toString();
    if (MyMoneySplit::ActionSplitShares == action)
      returnValue[id] = returnValue[id] * MyMoneyMoney(shares);
    else
      returnValue[id] += MyMoneyMoney(shares);
  }
  return returnValue;
}

void MyMoneyStorageSql::readTransactions(const QString& tidList, const QString& dateClause) {
  TRY
  m_storage->loadTransactions(fetchTransactions(tidList, dateClause));
  m_storage->loadTransactionId(m_hiIdTransactions);
  PASS
}

void MyMoneyStorageSql::readTransactions(const MyMoneyTransactionFilter& filter) {
  TRY
  m_storage->loadTransactions(fetchTransactions(filter));
  m_storage->loadTransactionId(m_hiIdTransactions);
  PASS
}

const QMap<QString, MyMoneyTransaction> MyMoneyStorageSql::fetchTransactions (const QString& tidList, const QString& dateClause, bool /*forUpdate*/) const {
  DBG("*** Entering MyMoneyStorageSql::fetchTransactions");
//  if (m_transactionListRead) return; // all list already in memory
  if (m_displayStatus) signalProgress(0, m_transactions, QObject::tr("Loading transactions..."));
  int progress = 0;
//  m_payeeList.clear();
  QString whereClause;
  whereClause = " WHERE txType = 'N' ";
  if (! tidList.isEmpty()) {
    whereClause += " AND id IN " + tidList;
  }
  if (!dateClause.isEmpty()) whereClause += " and " + dateClause;
  const MyMoneyDbTable& t = m_db.m_tables["kmmTransactions"];
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  q.prepare (t.selectAllString(false) + whereClause + " ORDER BY id;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Transaction")));
  const MyMoneyDbTable& ts = m_db.m_tables["kmmSplits"];
  whereClause = " WHERE txType = 'N' ";
  if (! tidList.isEmpty()) {
    whereClause += " AND transactionId IN " + tidList;
  }
  if (!dateClause.isEmpty()) whereClause += " and " + dateClause;
  MyMoneySqlQuery qs(const_cast <MyMoneyStorageSql*> (this));
  QString splitQuery = ts.selectAllString(false) + whereClause
      + " ORDER BY transactionId, splitId;";
  qs.prepare (splitQuery);
  if (!qs.exec()) throw new MYMONEYEXCEPTION(buildError (qs, __func__, "reading Splits"));
  QString splitTxId = "ZZZ";
  MyMoneySplit s;
  if (qs.next()) {
    splitTxId = qs.value(0).toString();
    readSplit (s, qs, ts);
  } else {
    splitTxId = "ZZZ";
  }
  QMap <QString, MyMoneyTransaction> txMap;
  QStringList txList;
  MyMoneyDbTable::field_iterator txEnd = t.end();
  while (q.next()) {
    MyMoneyTransaction tx;
    QString txId;
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    while (ft != txEnd) {
      CASE(id) txId = GETSTRING;
      else CASE(postDate) tx.setPostDate(GETDATE);
      else CASE(memo) tx.setMemo(GETSTRING);
      else CASE(entryDate) tx.setEntryDate(GETDATE);
      else CASE(currencyId) tx.setCommodity(GETSTRING);
      else CASE(bankId) tx.setBankID(GETSTRING);
      ++ft; ++i;
    }

    while (txId < splitTxId && splitTxId != "ZZZ") {
      if (qs.next()) {
        splitTxId = qs.value(0).toString();
        readSplit (s, qs, ts);
      } else {
        splitTxId = "ZZZ";
      }
    }

    while (txId == splitTxId) {
      tx.addSplit (s);
      if (qs.next()) {
        splitTxId = qs.value(0).toString();
        readSplit (s, qs, ts);
      } else {
        splitTxId = "ZZZ";
      }
    }
  // Process any key value pair
    if (! txId.isEmpty()) {
      txList.append(txId);
      tx = MyMoneyTransaction(txId, tx);
      txMap.insert(tx.uniqueSortKey(), tx);
    }
  }
  QMap <QString, MyMoneyKeyValueContainer> kvpMap = readKeyValuePairs("TRANSACTION", txList);
  QMap<QString, MyMoneyTransaction> tList;
  QMap<QString, MyMoneyTransaction>::Iterator txMapEnd = txMap.end();
  for (QMap<QString, MyMoneyTransaction>::Iterator i = txMap.begin();
       i != txMapEnd; ++i) {
         i.value().setPairs(kvpMap[i.value().id()].pairs());

         if (m_displayStatus) signalProgress(++progress, 0);
       }

       if ((tidList.isEmpty()) && (dateClause.isEmpty())) {
         //qDebug("setting full list read");
       }
  return txMap;
}

int MyMoneyStorageSql::splitState(const MyMoneyTransactionFilter::stateOptionE& state) const
{
  int rc = MyMoneySplit::NotReconciled;

  switch(state) {
    default:
    case MyMoneyTransactionFilter::notReconciled:
      break;

    case MyMoneyTransactionFilter::cleared:
      rc = MyMoneySplit::Cleared;
      break;

    case MyMoneyTransactionFilter::reconciled:
      rc = MyMoneySplit::Reconciled;
      break;

    case MyMoneyTransactionFilter::frozen:
      rc = MyMoneySplit::Frozen;
      break;
  }
  return rc;
}

const QMap<QString, MyMoneyTransaction> MyMoneyStorageSql::fetchTransactions (const MyMoneyTransactionFilter& filter) const {
  DBG("*** Entering MyMoneyStorageSql::fetchTransactions");
  // analyze the filter
//  if (m_transactionListRead) return; // all list already in memory
  // if the filter is restricted to certain accounts/categories
  // check if we already have them all in memory
  QStringList accounts;
  QString inQuery;
  filter.accounts(accounts);
  filter.categories(accounts);
//  QStringList::iterator it;
//  bool allAccountsLoaded = true;
//  for (it = accounts.begin(); it != accounts.end(); ++it) {
//    if (m_accountsLoaded.find(*it) == m_accountsLoaded.end()) {
//      allAccountsLoaded = false;
//      break;
//    }
//  }
//  if (allAccountsLoaded) return;
  /* Some filter combinations do not lend themselves to implementation
  * in SQL, or are likely to require such extensive reading of the database
  * as to make it easier to just read everything into memory.  */
  bool canImplementFilter = true;
  MyMoneyMoney m1, m2;
  if (filter.amountFilter( m1, m2 )) {
    alert ("Amount Filter Set");
    canImplementFilter = false;
  }
  QString n1, n2;
  if (filter.numberFilter(n1, n2)) {
    alert("Number filter set");
    canImplementFilter = false;
  }
  int t1;
  if (filter.firstType(t1)) {
    alert("Type filter set");
    canImplementFilter = false;
  }
//  int s1;
//  if (filter.firstState(s1)) {
//    alert("State filter set");
//    canImplementFilter = false;
//  }
  QRegExp t2;
  if (filter.textFilter(t2)) {
    alert("text filter set");
    canImplementFilter = false;
  }
  MyMoneyTransactionFilter::FilterSet s = filter.filterSet();
  if (s.singleFilter.validityFilter) {
    alert("Validity filter set");
    canImplementFilter = false;
  }
  if (!canImplementFilter) {
    QMap<QString, MyMoneyTransaction> transactionList =  fetchTransactions();
    QMap<QString, MyMoneyTransaction>::ConstIterator it_t;
    QMap<QString, MyMoneyTransaction>::ConstIterator txListEnd = transactionList.constEnd();

    std::remove_if(transactionList.begin(), transactionList.end(), FilterFail(filter, m_storagePtr));
    return transactionList;
  }

  bool accountsOnlyFilter = true;
  bool splitFilterActive = false; // the split filter is active if we are selecting on fields in the split table
  // get start and end dates
  QDate start = filter.fromDate();
  QDate end = filter.toDate();
  // not entirely sure if the following is correct, but at best, saves a lot of reads, at worst
  // it only causes us to read a few more transactions that strictly necessary (I think...)
  if (start == KMyMoneySettings::startDate().date()) start = QDate();
  bool txFilterActive = ((start != QDate()) || (end != QDate())); // and this for fields in the transaction table
  if (txFilterActive) accountsOnlyFilter = false;

  QString whereClause = "";
  QString subClauseconnector = " where txType = 'N' and ";
  // payees
  QStringList payees;
  //filter.payees(payees);
  if (filter.payees(payees)) {
    accountsOnlyFilter = false;
    QString itemConnector = "payeeId in (";
    QString payeesClause = "";
    QStringList::ConstIterator it;
    for (it = payees.constBegin(); it != payees.constEnd(); ++it) {
      payeesClause.append(QString("%1'%2'")
          .arg(itemConnector).arg(*it));
      itemConnector = ", ";
    }
    if (payeesClause.isEmpty())
    {
      whereClause += subClauseconnector + payeesClause + ")";
      subClauseconnector = " and ";
    }
    splitFilterActive = true;
  }

  // accounts and categories
  if (!accounts.isEmpty()) {
    splitFilterActive = true;
    QString itemConnector = "accountId in (";
    QString accountsClause = "";
    QStringList::ConstIterator it;
    for (it = accounts.constBegin(); it != accounts.constEnd(); ++it) {
//      if (m_accountsLoaded.find(*it) == m_accountsLoaded.end()) {
        accountsClause.append(QString("%1 '%2'")
            .arg(itemConnector).arg(*it));
        itemConnector = ", ";
        //if (accountsOnlyFilter) m_accountsLoaded.append(*it); // a bit premature...
//      }
    }
    if (!accountsClause.isEmpty()) {
      whereClause += subClauseconnector + accountsClause + ")";
      subClauseconnector = " and (";
    }
  }

  // split states
  QList <int> splitStates;
  if (filter.states(splitStates)) {
    splitFilterActive = true;
    QString itemConnector = " reconcileFlag IN (";
    QString statesClause = "";
    for (QList<int>::ConstIterator it = splitStates.constBegin(); it != splitStates.constEnd(); ++it) {
      statesClause.append(QString(" %1 '%2'").arg(itemConnector)
          .arg(splitState(MyMoneyTransactionFilter::stateOptionE(*it))));
      itemConnector = ",";
    }
    if (!statesClause.isEmpty()) {
      whereClause += subClauseconnector + statesClause + ")";
      subClauseconnector = " and (";
    }
  }
  // I've given up trying to work out the logic. we keep getting the wrong number of close brackets
  int obc = whereClause.count('(');
  int cbc = whereClause.count(')');
  if (cbc > obc) {
    qDebug() << "invalid where clause " << whereClause;
    qFatal("aborting");
  }
  while (cbc < obc) {
    whereClause.append(")");
    cbc++;
  }
  // if the split filter is active, but the where clause is empty
  // it means we already have all the transactions for the specified filter
  // in memory, so just exit
  if ((splitFilterActive) && (whereClause.isEmpty())) {
    qDebug("all transactions already in storage");
    return fetchTransactions();
  }

  // if we have neither a split filter, nor a tx (date) filter
  // it's effectively a read all
  if ((!splitFilterActive) && (!txFilterActive)) {
    //qDebug("reading all transactions");
    return fetchTransactions();
  }
  // build a date clause for the transaction table
  QString dateClause;
  QString connector = "";
  if (end != QDate()) {
    dateClause = QString("(postDate < '%1')").arg(end.addDays(1).toString(Qt::ISODate));
    connector = " and ";
  }
  if (start != QDate()) {
    dateClause += QString("%1 (postDate >= '%2')").arg(connector).arg(start.toString(Qt::ISODate));
  }
  // now get a list of transaction ids
  // if we have only a date filter, we need to build the list from the tx table
  // otherwise we need to build from the split table
  if (splitFilterActive) {
    inQuery = QString("(select distinct transactionId from kmmSplits %1)").arg(whereClause);
  } else {
    inQuery = QString("(select distinct id from kmmTransactions where %1)").arg(dateClause);
    txFilterActive = false; // kill off the date filter now
  }

  return fetchTransactions(inQuery, dateClause);
  //FIXME: if we have an accounts-only filter, recalc balances on loaded accounts
}

unsigned long MyMoneyStorageSql::transactionCount (const QString& aid) const {
  DBG("*** Entering MyMoneyStorageSql::transactionCount");
  if (aid.length() == 0)
    return m_transactions;
  else
    return m_transactionCountMap[aid];
}

void MyMoneyStorageSql::readSplit (MyMoneySplit& s, const MyMoneySqlQuery& q, const MyMoneyDbTable& t) const {
  DBG("*** Entering MyMoneyStorageSql::readSplit");
  s.clearId();
  MyMoneyDbTable::field_iterator ft = t.begin();
  MyMoneyDbTable::field_iterator splitEnd = t.end();
  int i = 0;

  // Use the QString here instead of CASE, since this is called so often.
  QString fieldName;
  while (ft != splitEnd) {
    fieldName = (*ft)->name();
    if (fieldName == "payeeId") s.setPayeeId(GETSTRING);
    else if (fieldName == "reconcileDate") s.setReconcileDate(GETDATE);
    else if (fieldName == "action") s.setAction(GETSTRING);
    else if (fieldName == "reconcileFlag") s.setReconcileFlag(static_cast<MyMoneySplit::reconcileFlagE>(GETINT));
    else if (fieldName == "value") s.setValue(MyMoneyMoney(QStringEmpty(GETSTRING)));
    else if (fieldName == "shares") s.setShares(MyMoneyMoney(QStringEmpty(GETSTRING)));
    else if (fieldName == "price") s.setPrice(MyMoneyMoney(QStringEmpty(GETSTRING)));
    else if (fieldName == "memo") s.setMemo(GETSTRING);
    else if (fieldName == "accountId") s.setAccountId(GETSTRING);
    else if (fieldName == "checkNumber") s.setNumber(GETSTRING);
    //else if (fieldName == "postDate") s.setPostDate(GETDATETIME); // FIXME - when Tom puts date into split object
    else if (fieldName == "bankId") s.setBankID(GETSTRING);
    ++ft; ++i;
  }

  return;
}

bool MyMoneyStorageSql::isReferencedByTransaction(const QString& id) const {
  DBG("*** Entering MyMoneyStorageSql::isReferencedByTransaction");
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  q.prepare("SELECT COUNT(*) FROM kmmTransactions "
        "INNER JOIN kmmSplits ON kmmTransactions.id = kmmSplits.transactionId "
         "WHERE kmmTransactions.currencyId = :ID OR kmmSplits.payeeId = :ID "
         "OR kmmSplits.accountId = :ID");
  q.bindValue(":ID", id);
  if ((!q.exec()) || (!q.next())) {
    buildError (q, __func__, "error retrieving reference count");
    qFatal("Error retrieving reference count"); // definitely shouldn't happen
  }
  return (0 != q.value(0).toULongLong());
}

void MyMoneyStorageSql::readSchedules(void) {

  TRY
  m_storage->loadSchedules(fetchSchedules());
  readFileInfo();
  m_storage->loadScheduleId(m_hiIdSchedules);
  PASS
}

const QMap<QString, MyMoneySchedule> MyMoneyStorageSql::fetchSchedules (const QStringList& idList, bool forUpdate) const {
  DBG("*** Entering MyMoneyStorageSql::fetchSchedules");
  signalProgress(0, m_schedules, QObject::tr("Loading schedules..."));
  int progress = 0;
  const MyMoneyDbTable& t = m_db.m_tables["kmmSchedules"];
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  QMap<QString, MyMoneySchedule> sList;
  //unsigned long lastId = 0;
  const MyMoneyDbTable& ts = m_db.m_tables["kmmSplits"];
  MyMoneySqlQuery qs(const_cast <MyMoneyStorageSql*> (this));
  qs.prepare (ts.selectAllString(false) + " WHERE transactionId = :id ORDER BY splitId;");
  MyMoneySqlQuery sq(const_cast <MyMoneyStorageSql*> (this));
  sq.prepare ("SELECT payDate from kmmSchedulePaymentHistory where schedId = :id");

  QString queryString (t.selectAllString(false));

  // Use bind variables, instead of just inserting the values in the queryString,
  // so that values containing a ':' will work.
  if (! idList.empty()) {
    queryString += " WHERE";
    for (int i = 0; i < idList.count(); ++i)
      queryString += " id = :id" + QString::number(i) + " OR";
    queryString = queryString.left(queryString.length() - 2);
  }
  queryString += " ORDER BY id;";

  if (forUpdate)
    queryString += " FOR UPDATE";

  queryString += ";";

  q.prepare (queryString);

  if (! idList.empty()) {
    QStringList::ConstIterator bindVal = idList.begin();
    for (int i = 0; bindVal != idList.end(); ++i, ++bindVal) {
      q.bindValue (":id" + QString::number(i), *bindVal);
    }
  }

  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Schedules")));
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    MyMoneySchedule s;
    QString sId;
    QString boolChar;
    QDate nextPaymentDue;
    while (ft != t.end()) {
      CASE(id) sId = GETSTRING;
      else CASE(name)  s.setName (GETSTRING);
      else CASE(type)  s.setType (static_cast<MyMoneySchedule::typeE>(GETINT));
      else CASE(occurence)  s.setOccurencePeriod (static_cast<MyMoneySchedule::occurenceE>(GETINT));
      else CASE(occurenceMultiplier) s.setOccurenceMultiplier (GETINT);
      else CASE(paymentType)  s.setPaymentType (static_cast<MyMoneySchedule::paymentTypeE>(GETINT));
      else CASE(startDate)  s.setStartDate (GETDATE);
      else CASE(endDate)  s.setEndDate (GETDATE);
      else CASE(fixed) {boolChar = GETSTRING; s.setFixed (boolChar == "Y");}
      else CASE(autoEnter)  {boolChar = GETSTRING; s.setAutoEnter (boolChar == "Y");}
      else CASE(lastPayment)  s.setLastPayment (GETDATE);
      else CASE(weekendOption)
        s.setWeekendOption (static_cast<MyMoneySchedule::weekendOptionE>(GETINT));
      else CASE(nextPaymentDue) nextPaymentDue = GETDATE;
      ++ft; ++i;
    }
    // convert simple occurence to compound occurence
    int mult = s.occurenceMultiplier();
    MyMoneySchedule::occurenceE occ = s.occurencePeriod();
    MyMoneySchedule::simpleToCompoundOccurence(mult,occ);
    s.setOccurencePeriod(occ);
    s.setOccurenceMultiplier(mult);
    // now assign the id to the schedule
    MyMoneySchedule _s(sId, s);
    s = _s;
    // read the associated transaction
//    m_payeeList.clear();
    const MyMoneyDbTable& t = m_db.m_tables["kmmTransactions"];
    MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
    q.prepare (t.selectAllString(false) + " WHERE id = :id;");
    q.bindValue(":id", s.id());
    if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Scheduled Transaction")));
    if (!q.next()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("retrieving scheduled transaction")));
    MyMoneyTransaction tx(s.id(), MyMoneyTransaction());
    ft = t.begin();
    i = 0;
    while (ft != t.end()) {
      CASE(postDate) tx.setPostDate(GETDATE);
      else CASE(memo) tx.setMemo(GETSTRING);
      else CASE(entryDate) tx.setEntryDate(GETDATE);
      else CASE(currencyId) tx.setCommodity(GETSTRING);
      else CASE(bankId) tx.setBankID(GETSTRING);
      ++ft; ++i;
    }

    qs.bindValue(":id", s.id());
    if (!qs.exec()) throw new MYMONEYEXCEPTION(buildError (qs, __func__, "reading Scheduled Splits"));
    while (qs.next()) {
      MyMoneySplit sp;
      readSplit (sp, qs, ts);
      tx.addSplit (sp);
    }
//    if (!m_payeeList.isEmpty())
//      readPayees(m_payeeList);
    // Process any key value pair
    tx.setPairs(readKeyValuePairs("TRANSACTION", s.id()).pairs());

    // If the transaction doesn't have a post date, setTransaction will reject it.
    // The old way of handling things was to store the next post date in the schedule object
    // and set the transaction post date to QDate().
    // For compatibility, if this is the case, copy the next post date from the schedule object
    // to the transaction object post date.
    if (!tx.postDate().isValid()) {
      tx.setPostDate(nextPaymentDue);
    }

    s.setTransaction(tx);

    // read in the recorded payments
    sq.bindValue(":id", s.id());
    if (!sq.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading schedule payment history")));
    while (sq.next()) s.recordPayment (sq.value(0).toDate());

    sList[s.id()] = s;

  //FIXME: enable when schedules have KVPs.
  //  s.setPairs(readKeyValuePairs("SCHEDULE", s.id()).pairs());

    //unsigned long id = extractId(s.id().data());
    //if(id > lastId)
    //  lastId = id;

    signalProgress(++progress, 0);
  }
  return sList;
}

void MyMoneyStorageSql::readSecurities(void) {
  TRY
  m_storage->loadSecurities(fetchSecurities());
  readFileInfo();
  m_storage->loadSecurityId(m_hiIdSecurities);
  PASS
}

const QMap<QString, MyMoneySecurity> MyMoneyStorageSql::fetchSecurities (const QStringList& /*idList*/, bool /*forUpdate*/) const {
  DBG("*** Entering MyMoneyStorageSql::fetchSecurities");
  signalProgress(0, m_securities, QObject::tr("Loading securities..."));
  int progress = 0;
  QMap<QString, MyMoneySecurity> sList;
  unsigned long lastId = 0;
  const MyMoneyDbTable& t = m_db.m_tables["kmmSecurities"];
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  q.prepare (t.selectAllString(false) + " ORDER BY id;");
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Securities")));
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    MyMoneySecurity e;
    QString eid;
    int saf = 0;
    while (ft != t.end()) {
      CASE(id) eid = GETSTRING;
      else CASE(name) e.setName(GETSTRING);
      else CASE(symbol) e.setTradingSymbol(GETSTRING);
      else CASE(type) e.setSecurityType(static_cast<MyMoneySecurity::eSECURITYTYPE>(GETINT));
      else CASE(smallestAccountFraction) saf = GETINT;
      else CASE(tradingCurrency) e.setTradingCurrency(GETSTRING);
      else CASE(tradingMarket) e.setTradingMarket(GETSTRING);
      ++ft; ++i;
    }
    if(e.tradingCurrency().isEmpty())
      e.setTradingCurrency(m_storage->pairs()["kmm-baseCurrency"]);
    if(saf == 0)
      saf = 100;
    e.setSmallestAccountFraction(saf);

  // Process any key value pairs
    e.setPairs(readKeyValuePairs("SECURITY", eid).pairs());
  //tell the storage objects we have a new security object.

    // FIXME: Adapt to new interface make sure, to take care of the currencies as well
    //   see MyMoneyStorageXML::readSecurites()
    MyMoneySecurity security(eid,e);
    sList[security.id()] = security;

    unsigned long id = extractId(security.id());
    if(id > lastId)
      lastId = id;

    signalProgress(++progress, 0);
  }
  return sList;
}

void MyMoneyStorageSql::readPrices(void) {

    TRY
//    m_storage->addPrice(MyMoneyPrice(from, to,  date, rate, source));
    PASS

}

const  MyMoneyPrice MyMoneyStorageSql::fetchSinglePrice (const QString& fromIdList, const QString& toIdList, const QDate& date_, bool exactDate, bool /*forUpdate*/) const {
  DBG("*** Entering MyMoneyStorageSql::fetchSinglePrice");
  const MyMoneyDbTable& t = m_db.m_tables["kmmPrices"];
  MyMoneyDbTable::field_iterator tableEnd = t.end();
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  QString queryString = t.selectAllString(false);

  // Use bind variables, instead of just inserting the values in the queryString,
  // so that values containing a ':' will work.
  // See balance query for why the date logic seems odd.
  queryString += " WHERE fromId = :fromId  AND toId = :toId AND priceDate < :priceDate ";
  if (exactDate)
    queryString += "AND priceDate > :exactDate ";

  queryString += "ORDER BY priceDate DESC;";

  q.prepare(queryString);

  QDate date (date_);

  if(!date.isValid())
    date = QDate::currentDate();

  q.bindValue(":fromId", fromIdList);
  q.bindValue(":toId", toIdList);
  q.bindValue(":priceDate", date.addDays(1).toString(Qt::ISODate));

  if (exactDate)
    q.bindValue(":exactDate", date.toString(Qt::ISODate));

  if (! q.exec()) {}

  if (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QString from;
    QString to;
    QDate date;
    MyMoneyMoney rate;
    QString source;
    bool foundFromId = false;
    bool foundToId = false;
    bool foundPriceDate = false;
    bool foundPrice = false;
    bool foundPriceSource = false;
    while (ft != tableEnd) {
      bool foundSomething = false;
      if (!foundFromId && !foundSomething) {
        CASE(fromId) {from = GETSTRING; foundFromId = true; foundSomething = true;}
      }
      if (!foundToId && !foundSomething) {
        CASE(toId) {to = GETSTRING; foundToId = true; foundSomething = true;}
      }
      if (!foundPriceDate && !foundSomething) {
        CASE(priceDate) {date = GETDATE; foundPriceDate = true; foundSomething = true;}
      }
      if (!foundPrice && !foundSomething) {
        CASE(price) {rate = GETSTRING; foundPrice = true; foundSomething = true;}
      }
      if (!foundPriceSource && !foundSomething) {
        CASE(priceSource) {source = GETSTRING; foundPriceSource = true; foundSomething = true;}
      }
      ++ft; ++i;
    }

    return MyMoneyPrice(fromIdList, toIdList, date, rate, source);
  }

  return MyMoneyPrice();
}

const  MyMoneyPriceList MyMoneyStorageSql::fetchPrices (const QStringList& fromIdList, const QStringList& toIdList,  bool forUpdate) const {
  DBG("*** Entering MyMoneyStorageSql::fetchPrices");
  signalProgress(0, m_prices, QObject::tr("Loading prices..."));
  int progress = 0;
  const_cast <MyMoneyStorageSql*> (this)->m_readingPrices = true;
  MyMoneyPriceList pList;
  const MyMoneyDbTable& t = m_db.m_tables["kmmPrices"];
  MyMoneyDbTable::field_iterator tableEnd = t.end();
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  QString queryString = t.selectAllString(false);

  // Use bind variables, instead of just inserting the values in the queryString,
  // so that values containing a ':' will work.
  if (! fromIdList.empty()) {
    queryString += " WHERE (";
    for (int i = 0; i < fromIdList.count(); ++i) {
      queryString += " fromId = :fromId" + QString::number(i) + " OR";
    }
    queryString = queryString.left(queryString.length() - 2) + ")";
  }
  if (! toIdList.empty()) {
    queryString += " AND (";
    for (int i = 0; i < toIdList.count(); ++i) {
      queryString += " toId = :toId" + QString::number(i) + " OR";
    }
    queryString = queryString.left(queryString.length() - 2) + ")";
  }


  if (forUpdate)
    queryString += " FOR UPDATE";

  queryString += ";";

  q.prepare (queryString);

  if (! fromIdList.empty()) {
    QStringList::ConstIterator bindVal = fromIdList.begin();
    for (int i = 0; bindVal != fromIdList.end(); ++i, ++bindVal) {
      q.bindValue (":fromId" + QString::number(i), *bindVal);
    }
  }
  if (! toIdList.empty()) {
    QStringList::ConstIterator bindVal = toIdList.begin();
    for (int i = 0; bindVal != toIdList.end(); ++i, ++bindVal) {
      q.bindValue (":toId" + QString::number(i), *bindVal);
    }
  }

  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Prices")));
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QString from;
    QString to;
    QDate date;
    MyMoneyMoney rate;
    QString source;

    while (ft != tableEnd) {
      CASE(fromId) from = GETSTRING;
      else CASE(toId) to = GETSTRING;
      else CASE(priceDate) date = GETDATE;
      else CASE(price) rate = GETSTRING;
      else CASE(priceSource) source = GETSTRING;
      ++ft; ++i;
    }
    pList [MyMoneySecurityPair(from, to)].insert(date, MyMoneyPrice(from, to,  date, rate, source));
    signalProgress(++progress, 0);
  }
  const_cast <MyMoneyStorageSql*> (this)->m_readingPrices = false;

  return pList;
}

void MyMoneyStorageSql::readCurrencies(void) {
  TRY
  m_storage->loadCurrencies(fetchCurrencies());
  PASS
}

const QMap<QString, MyMoneySecurity> MyMoneyStorageSql::fetchCurrencies (const QStringList& idList, bool forUpdate) const {
  DBG("*** Entering MyMoneyStorageSql::fetchCurrencies");
  signalProgress(0, m_currencies, QObject::tr("Loading currencies..."));
  int progress = 0;
  QMap<QString, MyMoneySecurity> cList;
  const MyMoneyDbTable& t = m_db.m_tables["kmmCurrencies"];
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));

  QString queryString (t.selectAllString(false));

  // Use bind variables, instead of just inserting the values in the queryString,
  // so that values containing a ':' will work.
  if (! idList.empty()) {
    queryString += " WHERE";
    for (int i = 0; i < idList.count(); ++i)
      queryString += " isocode = :id" + QString::number(i) + " OR";
    queryString = queryString.left(queryString.length() - 2);
  }

  queryString += " ORDER BY ISOcode";

  if (forUpdate)
    queryString += " FOR UPDATE";

  queryString += ";";

  q.prepare (queryString);

  if (! idList.empty()) {
    QStringList::ConstIterator bindVal = idList.begin();
    for (int i = 0; bindVal != idList.end(); ++i, ++bindVal) {
      q.bindValue (":id" + QString::number(i), *bindVal);
    }
  }

  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Currencies")));
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QString id;
    MyMoneySecurity c;
    QChar symbol[3];
    while (ft != t.end()) {
      CASE(ISOcode) id = GETSTRING;
      else CASE(name) c.setName(GETSTRING);
      else CASE(type) c.setSecurityType(static_cast<MyMoneySecurity::eSECURITYTYPE>(GETINT));
      else CASE(symbol1) symbol[0] = QChar(GETINT);
      else CASE(symbol2) symbol[1] = QChar(GETINT);
      else CASE(symbol3) symbol[2] = QChar(GETINT);
      else CASE(partsPerUnit) c.setPartsPerUnit(GETINT);
      else CASE(smallestCashFraction) c.setSmallestCashFraction(GETINT);
      else CASE(smallestAccountFraction) c.setSmallestAccountFraction(GETINT);
      ++ft; ++i;
    }
    c.setTradingSymbol(QString(symbol, 3).trimmed());

    cList[id] = MyMoneySecurity(id, c);

    signalProgress(++progress, 0);
  }
  return cList;
}

void MyMoneyStorageSql::readReports(void) {
  TRY
  m_storage->loadReports(fetchReports());
  readFileInfo();
  m_storage->loadReportId(m_hiIdReports);
  PASS
}

const QMap<QString, MyMoneyReport> MyMoneyStorageSql::fetchReports (const QStringList& /*idList*/, bool /*forUpdate*/) const {
  DBG("*** Entering MyMoneyStorageSql::fetchReports");
  signalProgress(0, m_reports, QObject::tr("Loading reports..."));
  int progress = 0;
  const MyMoneyDbTable& t = m_db.m_tables["kmmReportConfig"];
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  q.prepare (t.selectAllString(true));
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading reports")));
  QMap<QString, MyMoneyReport> rList;
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QDomDocument d;
    while (ft != t.end()) {
      CASE(XML) d.setContent(GETSTRING, false);
      ++ft; ++i;
    }
    QDomNode child = d.firstChild();
    child = child.firstChild();
    MyMoneyReport report;

    if (report.read(child.toElement()))
      rList[report.id()] = report;

    signalProgress(++progress, 0);
  }
  return rList;
}

const QMap<QString, MyMoneyBudget> MyMoneyStorageSql::fetchBudgets (const QStringList& idList, bool forUpdate) const {
  DBG("*** Entering MyMoneyStorageSql::fetchBudgets");
  signalProgress(0, m_budgets, QObject::tr("Loading budgets..."));
  int progress = 0;
  const MyMoneyDbTable& t = m_db.m_tables["kmmBudgetConfig"];
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  QString queryString (t.selectAllString(false));
  if (! idList.empty()) {
    queryString += " WHERE id = '" + idList.join("' OR id = '") + "'";
  }
  if (forUpdate)
    queryString += " FOR UPDATE";

  queryString += ";";

  q.prepare (queryString);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading budgets")));
  QMap<QString, MyMoneyBudget> budgets;
  while (q.next()) {
    MyMoneyDbTable::field_iterator ft = t.begin();
    int i = 0;
    QDomDocument d;
    while (ft != t.end()) {
      CASE(XML) d.setContent(GETSTRING, false);
      ++ft; ++i;
    }
    QDomNode child = d.firstChild();
    child = child.firstChild();
    MyMoneyBudget budget (child.toElement());
    budgets.insert(budget.id(), budget);
    signalProgress(++progress, 0);
  }
  return budgets;
}

void MyMoneyStorageSql::readBudgets(void) {
  m_storage->loadBudgets(fetchBudgets());
}

const MyMoneyKeyValueContainer MyMoneyStorageSql::readKeyValuePairs (const QString& kvpType, const QString& kvpId) const {
  DBG("*** Entering MyMoneyStorageSql::readKeyValuePairs");
  MyMoneyKeyValueContainer list;
  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  q.prepare ("SELECT kvpKey, kvpData from kmmKeyValuePairs where kvpType = :type and kvpId = :id;");
  q.bindValue(":type", kvpType);
  q.bindValue(":id", kvpId);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Kvp for %1 %2").arg(kvpType)
      .arg(kvpId)));
  while (q.next()) list.setValue(q.value(0).toString(), q.value(1).toString());
  return (list);
}

const QMap<QString, MyMoneyKeyValueContainer> MyMoneyStorageSql::readKeyValuePairs (const QString& kvpType, const QStringList& kvpIdList) const {
  DBG("*** Entering MyMoneyStorageSql::readKeyValuePairs");
  QMap<QString, MyMoneyKeyValueContainer> retval;

  MyMoneySqlQuery q(const_cast <MyMoneyStorageSql*> (this));
  QString query ("SELECT kvpId, kvpKey, kvpData from kmmKeyValuePairs where kvpType = :type");

  if (!kvpIdList.empty()) {
    query += " and kvpId IN ('" + kvpIdList.join("', '") + "')";
  }

  query += " order by kvpId;";
  q.prepare (query);
  q.bindValue(":type", kvpType);
  if (!q.exec()) throw new MYMONEYEXCEPTION(buildError (q, __func__, QString("reading Kvp List for %1").arg(kvpType)));
  while (q.next()) {
    retval [q.value(0).toString()].setValue(q.value(1).toString(), q.value(2).toString());
  }

  return (retval);
}

long unsigned MyMoneyStorageSql::getNextBudgetId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdBudgets;
}

long unsigned MyMoneyStorageSql::getNextAccountId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdAccounts;
}

long unsigned MyMoneyStorageSql::getNextInstitutionId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdInstitutions;
}

long unsigned MyMoneyStorageSql::getNextPayeeId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdPayees;
}

long unsigned MyMoneyStorageSql::getNextReportId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdReports;
}

long unsigned MyMoneyStorageSql::getNextScheduleId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdSchedules;
}

long unsigned MyMoneyStorageSql::getNextSecurityId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdSecurities;
}

long unsigned MyMoneyStorageSql::getNextTransactionId() const {
  const_cast <MyMoneyStorageSql*> (this)->readFileInfo();
  return m_hiIdTransactions;
}

long unsigned MyMoneyStorageSql::incrementBudgetId() {
 MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiBudgetId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiBudgetId = " + QString::number(returnValue));
  q.exec();
  m_hiIdBudgets = returnValue;
  return returnValue;
}

long unsigned MyMoneyStorageSql::incrementAccountId() {
  MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiAccountId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiAccountId = " + QString::number(returnValue));
  q.exec();
  m_hiIdAccounts = returnValue;
  return returnValue;
}

long unsigned MyMoneyStorageSql::incrementInstitutionId() {
  MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiInstitutionId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiInstitutionId = " + QString::number(returnValue));
  q.exec();
  m_hiIdInstitutions = returnValue;
  return returnValue;
}

long unsigned MyMoneyStorageSql::incrementPayeeId() {
  MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiPayeeId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiPayeeId = " + QString::number(returnValue));
  q.exec();
  m_hiIdPayees = returnValue;
  return returnValue;
}

long unsigned MyMoneyStorageSql::incrementReportId() {
  MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiReportId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiReportId = " + QString::number(returnValue));
  q.exec();
  m_hiIdReports = returnValue;
  return returnValue;
}

long unsigned MyMoneyStorageSql::incrementScheduleId() {
  MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiScheduleId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiScheduleId = " + QString::number(returnValue));
  q.exec();
  m_hiIdSchedules = returnValue;
  return returnValue;
}

long unsigned MyMoneyStorageSql::incrementSecurityId() {
  MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiSecurityId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiSecurityId = " + QString::number(returnValue));
  q.exec();
  m_hiIdSecurities = returnValue;
  return returnValue;
}

long unsigned MyMoneyStorageSql::incrementTransactionId() {
  MyMoneySqlQuery q(this);

  MyMoneyDbTransaction t(*this, __func__);
  q.prepare("SELECT hiTransactionId FROM kmmFileInfo FOR UPDATE");
  q.exec();
  q.next();
  long unsigned returnValue = (unsigned long) q.value(0).toULongLong();
  ++returnValue;
  q.prepare("UPDATE kmmFileInfo SET hiTransactionId = " + QString::number(returnValue));
  q.exec();
  m_hiIdTransactions = returnValue;
  return returnValue;
}

void MyMoneyStorageSql::loadAccountId(const unsigned long& id)
{
  m_hiIdAccounts = id;
  writeFileInfo();
}

void MyMoneyStorageSql::loadTransactionId(const unsigned long& id)
{
  m_hiIdTransactions = id;
  writeFileInfo();
}

void MyMoneyStorageSql::loadPayeeId(const unsigned long& id)
{
  m_hiIdPayees = id;
  writeFileInfo();
}

void MyMoneyStorageSql::loadInstitutionId(const unsigned long& id)
{
  m_hiIdInstitutions = id;
  writeFileInfo();
}

void MyMoneyStorageSql::loadScheduleId(const unsigned long& id)
{
  m_hiIdSchedules = id;
  writeFileInfo();
}

void MyMoneyStorageSql::loadSecurityId(const unsigned long& id)
{
  m_hiIdSecurities = id;
  writeFileInfo();
}

void MyMoneyStorageSql::loadReportId(const unsigned long& id)
{
  m_hiIdReports = id;
  writeFileInfo();
}

void MyMoneyStorageSql::loadBudgetId(const unsigned long& id)
{
  m_hiIdBudgets = id;
  writeFileInfo();
}

//****************************************************
long unsigned MyMoneyStorageSql::calcHighId
     (const long unsigned& i, const QString& id) {
  DBG("*** Entering MyMoneyStorageSql::calcHighId");
  QString nid = id;
  long unsigned high = (unsigned long) nid.replace(QRegExp("[A-Z]*"), "").toULongLong();
  return std::max(high, i);
}

void MyMoneyStorageSql::setProgressCallback(void(*callback)(int, int, const QString&)) {
  m_progressCallback = callback;
}

void MyMoneyStorageSql::signalProgress(int current, int total, const QString& msg) const {
  if (m_progressCallback != 0)
    (*m_progressCallback)(current, total, msg);
}

// **************************** Error display routine *******************************
QString& MyMoneyStorageSql::buildError (const QSqlQuery& q, const QString& function,
                                        const QString& messageb) const {
  return (buildError(q, function, messageb, this));
}

QString& MyMoneyStorageSql::buildError (const QSqlQuery& q, const QString& function,
                                        const QString& message, const QSqlDatabase* db) const {
  QString s = QString("Error in function %1 : %2").arg(function).arg(message);
  s += QString ("\nDriver = %1, Host = %2, User = %3, Database = %4")
      .arg(db->driverName()).arg(db->hostName()).arg(db->userName()).arg(db->databaseName());
  QSqlError e = db->lastError();
  s += QString ("\nDriver Error: %1").arg(e.driverText());
  s += QString ("\nDatabase Error No %1: %2").arg(e.number()).arg(e.databaseText());
  s += QString ("\nText: %1").arg(e.text());
  s += QString ("\nError type %1").arg(e.type());
  e = q.lastError();
  s += QString ("\nExecuted: %1").arg(q.executedQuery());
  s += QString ("\nQuery error No %1: %2").arg(e.number()).arg(e.text());
  s += QString ("\nError type %1").arg(e.type());

  const_cast <MyMoneyStorageSql*> (this)->m_error = s;
  qDebug("%s", qPrintable(s));
  const_cast <MyMoneyStorageSql*> (this)->cancelCommitUnit(function);
  return (const_cast <MyMoneyStorageSql*> (this)->m_error);
}

// ************************* Build table descriptions ****************************
MyMoneyDbDef::MyMoneyDbDef () {
  FileInfo();
  Institutions();
  Payees();
  Accounts();
  Transactions();
  Splits();
  KeyValuePairs();
  Schedules();
  SchedulePaymentHistory();
  Securities();
  Prices();
  Currencies();
  Reports();
  Budgets();
  Balances();
}

/* PRIMARYKEY - these fields combine to form a unique key field on which the db will create an index
   NOTNULL - this field should never be null
   UNSIGNED - for numeric types, indicates the field is UNSIGNED
   ?ISKEY - where there is no primary key, these fields can be used to uniquely identify a record
  Default is that a field is not a part of a primary key, nullable, and if numeric, signed */

#define PRIMARYKEY true
#define NOTNULL true
#define UNSIGNED false
//#define ISKEY true

#define appendField(a) fields.append(KSharedPtr<MyMoneyDbColumn>(new a))
void MyMoneyDbDef::FileInfo(void){
  QList< KSharedPtr<MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("version", "varchar(16)"));
  appendField(MyMoneyDbColumn("created", "date"));
  appendField(MyMoneyDbColumn("lastModified", "date"));
  appendField(MyMoneyDbColumn("baseCurrency", "char(3)"));
  appendField(MyMoneyDbIntColumn("institutions", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("accounts", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("payees", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("transactions", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("splits", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("securities", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("prices", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("currencies", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("schedules", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("reports", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("kvps", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbColumn("dateRangeStart", "date"));
  appendField(MyMoneyDbColumn("dateRangeEnd", "date"));
  appendField(MyMoneyDbIntColumn("hiInstitutionId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("hiPayeeId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("hiAccountId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("hiTransactionId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("hiScheduleId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("hiSecurityId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("hiReportId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbColumn("encryptData", "varchar(255)"));
  appendField(MyMoneyDbColumn("updateInProgress", "char(1)"));
  appendField(MyMoneyDbIntColumn("budgets", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbIntColumn("hiBudgetId", MyMoneyDbIntColumn::BIG, UNSIGNED));
  appendField(MyMoneyDbColumn("logonUser", "varchar(255)"));
  appendField(MyMoneyDbDatetimeColumn("logonAt"));
  appendField(MyMoneyDbIntColumn("fixLevel",
               MyMoneyDbIntColumn::MEDIUM, UNSIGNED));
  MyMoneyDbTable t("kmmFileInfo", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Institutions(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("id", "varchar(32)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbTextColumn("name", MyMoneyDbTextColumn::NORMAL, false, NOTNULL));
  appendField(MyMoneyDbTextColumn("manager"));
  appendField(MyMoneyDbTextColumn("routingCode"));
  appendField(MyMoneyDbTextColumn("addressStreet"));
  appendField(MyMoneyDbTextColumn("addressCity"));
  appendField(MyMoneyDbTextColumn("addressZipcode"));
  appendField(MyMoneyDbTextColumn("telephone"));
  MyMoneyDbTable t("kmmInstitutions", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Payees(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("id", "varchar(32)",  PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbTextColumn("name"));
  appendField(MyMoneyDbTextColumn("reference"));
  appendField(MyMoneyDbTextColumn("email"));
  appendField(MyMoneyDbTextColumn("addressStreet"));
  appendField(MyMoneyDbTextColumn("addressCity"));
  appendField(MyMoneyDbTextColumn("addressZipcode"));
  appendField(MyMoneyDbTextColumn("addressState"));
  appendField(MyMoneyDbTextColumn("telephone"));
  appendField(MyMoneyDbTextColumn("notes", MyMoneyDbTextColumn::LONG));
  appendField(MyMoneyDbColumn("defaultAccountId", "varchar(32)"));
  appendField(MyMoneyDbIntColumn("matchData", MyMoneyDbIntColumn::TINY, UNSIGNED));
  appendField(MyMoneyDbColumn("matchIgnoreCase", "char(1)"));
  appendField(MyMoneyDbTextColumn("matchKeys"));
  MyMoneyDbTable t("kmmPayees", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Accounts(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("id", "varchar(32)",  PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("institutionId", "varchar(32)"));
  appendField(MyMoneyDbColumn("parentId", "varchar(32)"));
  appendField(MyMoneyDbDatetimeColumn("lastReconciled"));
  appendField(MyMoneyDbDatetimeColumn("lastModified"));
  appendField(MyMoneyDbColumn("openingDate", "date"));
  appendField(MyMoneyDbTextColumn("accountNumber"));
  appendField(MyMoneyDbColumn("accountType", "varchar(16)", false, NOTNULL));
  appendField(MyMoneyDbTextColumn("accountTypeString"));
  appendField(MyMoneyDbColumn("isStockAccount", "char(1)"));
  appendField(MyMoneyDbTextColumn("accountName"));
  appendField(MyMoneyDbTextColumn("description"));
  appendField(MyMoneyDbColumn("currencyId", "varchar(32)"));
  appendField(MyMoneyDbTextColumn("balance"));
  appendField(MyMoneyDbTextColumn("balanceFormatted"));
  appendField(MyMoneyDbIntColumn("transactionCount", MyMoneyDbIntColumn::BIG, UNSIGNED));
  MyMoneyDbTable t("kmmAccounts", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Transactions(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("id", "varchar(32)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("txType", "char(1)"));
  appendField(MyMoneyDbDatetimeColumn("postDate"));
  appendField(MyMoneyDbTextColumn("memo"));
  appendField(MyMoneyDbDatetimeColumn("entryDate"));
  appendField(MyMoneyDbColumn("currencyId", "char(3)"));
  appendField(MyMoneyDbTextColumn("bankId"));
  MyMoneyDbTable t("kmmTransactions", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Splits(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("transactionId", "varchar(32)",  PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("txType", "char(1)"));
  appendField(MyMoneyDbIntColumn("splitId", MyMoneyDbIntColumn::SMALL, UNSIGNED,  PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("payeeId", "varchar(32)"));
  appendField(MyMoneyDbDatetimeColumn("reconcileDate"));
  appendField(MyMoneyDbColumn("action", "varchar(16)"));
  appendField(MyMoneyDbColumn("reconcileFlag", "char(1)"));
  appendField(MyMoneyDbTextColumn("value", MyMoneyDbTextColumn::NORMAL, false, NOTNULL));
  appendField(MyMoneyDbColumn("valueFormatted", "text"));
  appendField(MyMoneyDbTextColumn("shares", MyMoneyDbTextColumn::NORMAL, false, NOTNULL));
  appendField(MyMoneyDbTextColumn("sharesFormatted"));
  appendField(MyMoneyDbTextColumn("price", MyMoneyDbTextColumn::NORMAL, false));
  appendField(MyMoneyDbTextColumn("priceFormatted"));
  appendField(MyMoneyDbTextColumn("memo"));
  appendField(MyMoneyDbColumn("accountId", "varchar(32)", false, NOTNULL));
  appendField(MyMoneyDbColumn("checkNumber", "varchar(32)"));
  appendField(MyMoneyDbDatetimeColumn("postDate"));
  appendField(MyMoneyDbTextColumn("bankId"));
  MyMoneyDbTable t("kmmSplits", fields);
  QStringList list;
  list << "accountId" << "txType";
  t.addIndex("kmmSplitsaccount_type", list, false);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::KeyValuePairs(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("kvpType", "varchar(16)", false, NOTNULL));
  appendField(MyMoneyDbColumn("kvpId", "varchar(32)"));
  appendField(MyMoneyDbColumn("kvpKey", "varchar(255)", false, NOTNULL));
  appendField(MyMoneyDbTextColumn("kvpData"));
  MyMoneyDbTable t("kmmKeyValuePairs", fields);
  QStringList list;
  list << "kvpType" << "kvpId";
  t.addIndex("type_id", list, false);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Schedules(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("id", "varchar(32)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbTextColumn("name", MyMoneyDbTextColumn::NORMAL, false, NOTNULL));
  appendField(MyMoneyDbIntColumn("type", MyMoneyDbIntColumn::TINY, UNSIGNED, false, NOTNULL));
  appendField(MyMoneyDbTextColumn("typeString"));
  appendField(MyMoneyDbIntColumn("occurence", MyMoneyDbIntColumn::SMALL, UNSIGNED, false,
        NOTNULL));
  appendField(MyMoneyDbIntColumn("occurenceMultiplier", MyMoneyDbIntColumn::SMALL, UNSIGNED,
        false, NOTNULL));
  appendField(MyMoneyDbTextColumn("occurenceString"));
  appendField(MyMoneyDbIntColumn("paymentType", MyMoneyDbIntColumn::TINY, UNSIGNED));
  appendField(MyMoneyDbTextColumn("paymentTypeString", MyMoneyDbTextColumn::LONG));
  appendField(MyMoneyDbColumn("startDate", "date", false, NOTNULL));
  appendField(MyMoneyDbColumn("endDate", "date"));
  appendField(MyMoneyDbColumn("fixed", "char(1)", false, NOTNULL));
  appendField(MyMoneyDbColumn("autoEnter", "char(1)", false, NOTNULL));
  appendField(MyMoneyDbColumn("lastPayment", "date"));
  appendField(MyMoneyDbColumn("nextPaymentDue", "date"));
  appendField(MyMoneyDbIntColumn("weekendOption", MyMoneyDbIntColumn::TINY, UNSIGNED, false,
        NOTNULL));
  appendField(MyMoneyDbTextColumn("weekendOptionString"));
  MyMoneyDbTable t("kmmSchedules", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::SchedulePaymentHistory(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("schedId", "varchar(32)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("payDate", "date", PRIMARYKEY,  NOTNULL));
  MyMoneyDbTable t("kmmSchedulePaymentHistory", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Securities(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("id", "varchar(32)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("name", "text", false, NOTNULL));
  appendField(MyMoneyDbTextColumn("symbol"));
  appendField(MyMoneyDbIntColumn("type", MyMoneyDbIntColumn::SMALL, UNSIGNED, false, NOTNULL));
  appendField(MyMoneyDbTextColumn("typeString"));
  appendField(MyMoneyDbColumn("smallestAccountFraction", "varchar(24)"));
  appendField(MyMoneyDbTextColumn("tradingMarket"));
  appendField(MyMoneyDbColumn("tradingCurrency", "char(3)"));
  MyMoneyDbTable t("kmmSecurities", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Prices(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("fromId", "varchar(32)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("toId", "varchar(32)",  PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("priceDate", "date", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbTextColumn("price", MyMoneyDbTextColumn::NORMAL, false, NOTNULL));
  appendField(MyMoneyDbTextColumn("priceFormatted"));
  appendField(MyMoneyDbTextColumn("priceSource"));
  MyMoneyDbTable t("kmmPrices", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Currencies(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("ISOcode", "char(3)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbTextColumn("name", MyMoneyDbTextColumn::NORMAL, false, NOTNULL));
  appendField(MyMoneyDbIntColumn("type", MyMoneyDbIntColumn::SMALL, UNSIGNED));
  appendField(MyMoneyDbTextColumn("typeString"));
  appendField(MyMoneyDbIntColumn("symbol1", MyMoneyDbIntColumn::SMALL, UNSIGNED));
  appendField(MyMoneyDbIntColumn("symbol2", MyMoneyDbIntColumn::SMALL, UNSIGNED));
  appendField(MyMoneyDbIntColumn("symbol3", MyMoneyDbIntColumn::SMALL, UNSIGNED));
  appendField(MyMoneyDbColumn("symbolString", "varchar(255)"));
  appendField(MyMoneyDbColumn("partsPerUnit", "varchar(24)"));
  appendField(MyMoneyDbColumn("smallestCashFraction", "varchar(24)"));
  appendField(MyMoneyDbColumn("smallestAccountFraction", "varchar(24)"));
  MyMoneyDbTable t("kmmCurrencies", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Reports(void) {
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("name", "varchar(255)", false, NOTNULL));
  appendField(MyMoneyDbTextColumn("XML", MyMoneyDbTextColumn::LONG));
  appendField(MyMoneyDbColumn("id", "varchar(32)", PRIMARYKEY, NOTNULL));
  MyMoneyDbTable t("kmmReportConfig", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Budgets(void){
  QList<KSharedPtr <MyMoneyDbColumn> > fields;
  appendField(MyMoneyDbColumn("id", "varchar(32)", PRIMARYKEY, NOTNULL));
  appendField(MyMoneyDbColumn("name", "text", false, NOTNULL));
  appendField(MyMoneyDbColumn("start", "date", false, NOTNULL));
  appendField(MyMoneyDbTextColumn("XML", MyMoneyDbTextColumn::LONG) );
  MyMoneyDbTable t("kmmBudgetConfig", fields);
  t.buildSQLStrings();
  m_tables[t.name()] = t;
}

void MyMoneyDbDef::Balances(void){
  MyMoneyDbView v("kmmBalances", "CREATE VIEW kmmBalances AS "
    "SELECT kmmAccounts.id AS id, kmmAccounts.currencyId, "
    "kmmSplits.txType, kmmSplits.value, kmmSplits.shares, "
    "kmmSplits.postDate AS balDate, "
    "kmmTransactions.currencyId AS txCurrencyId "
    "FROM kmmAccounts, kmmSplits, kmmTransactions "
    "WHERE kmmSplits.txType = 'N' "
    "AND kmmSplits.accountId = kmmAccounts.id "
    "AND kmmSplits.transactionId = kmmTransactions.id;");
  m_views[v.name()] = v;
}

// function to write create SQL to a stream
const QString MyMoneyDbDef::generateSQL (const QString& driver) const {
  QString retval;
  databaseTypeE dbType = m_drivers.driverToType(driver);
  QMap<QString, MyMoneyDbTable>::ConstIterator tt = m_tables.begin();
  while (tt != m_tables.end()) {
    retval += (*tt).generateCreateSQL(dbType) + '\n';
    ++tt;
  }
  return retval;
}

//*****************************************************************************

void MyMoneyDbTable::addIndex(const QString& name, const QStringList& columns, bool unique) {
  m_indices.push_back (MyMoneyDbIndex (m_name, name, columns, unique));
}

void MyMoneyDbTable::buildSQLStrings (void) {
  // build fixed SQL strings for this table
  // build the insert string with placeholders for each field
  QString qs = QString("INSERT INTO %1 (").arg(name());
  QString ws = ") VALUES (";
  field_iterator ft = m_fields.constBegin();
  while (ft != m_fields.constEnd()) {
    qs += QString("%1, ").arg((*ft)->name());
    ws += QString(":%1, ").arg((*ft)->name());
    ++ft;
  }
  qs = qs.left(qs.length() - 2);
  ws = ws.left(ws.length() - 2);
  m_insertString = qs + ws + ");";
  // build a 'select all' string (select * is deprecated)
  // don't terminate with semicolon coz we may want a where or order clause
  m_selectAllString = "SELECT " + columnList() + " FROM " + name();;

  // build an update string; key fields go in the where clause
  qs = "UPDATE " + name() + " SET ";
  ws = QString();
  ft = m_fields.constBegin();
  while (ft != m_fields.constEnd()) {
    if ((*ft)->isPrimaryKey()) {
      if (!ws.isEmpty()) ws += " AND ";
      ws += QString("%1 = :%2").arg((*ft)->name()).arg((*ft)->name());
    } else {
      qs += QString("%1 = :%2, ").arg((*ft)->name()).arg((*ft)->name());
    }
    ++ft;
  }
  qs = qs.left(qs.length() - 2);
  if (!ws.isEmpty()) qs += " WHERE " + ws;
  m_updateString = qs + ";";
  // build a delete string; where clause as for update
  qs = "DELETE FROM " + name();
  if (!ws.isEmpty()) qs += " WHERE " + ws;
  m_deleteString = qs + ";";
 }

const QString MyMoneyDbTable::columnList() const {
  field_iterator ft = m_fields.begin();
  QString qs;
  ft = m_fields.begin();
  while (ft != m_fields.end()) {
    qs += QString("%1, ").arg((*ft)->name());
    ++ft;
  }
  return (qs.left(qs.length() - 2));
}

const QString MyMoneyDbTable::generateCreateSQL (databaseTypeE dbType) const {
  QString qs = QString("CREATE TABLE %1 (").arg(name());
  QString pkey;
  for (field_iterator it = m_fields.begin(); it != m_fields.end(); ++it) {
    qs += (*it)->generateDDL (dbType) + ", ";
    if ((*it)->isPrimaryKey ())
      pkey += (*it)->name () + ", ";
  }

  if (!pkey.isEmpty()) {
    qs += "PRIMARY KEY (" + pkey;
    qs = qs.left(qs.length() - 2) + "))";
  } else {
    qs = qs.left(qs.length() - 2) + ")";
  }

  if (dbType == Mysql)
    qs += " ENGINE = InnoDB;\n";
  else
    qs += ";\n";

  for (index_iterator ii = m_indices.begin(); ii != m_indices.end(); ++ii) {
    qs += (*ii).generateDDL(dbType);
  }
  return qs;
}

const QString MyMoneyDbTable::dropPrimaryKeyString(databaseTypeE dbType) const
{
  if (dbType == Mysql || dbType == Oracle8)
    return "ALTER TABLE " + m_name + " DROP PRIMARY KEY;";
  else if (dbType == Postgresql)
    return "ALTER TABLE " + m_name + " DROP CONSTRAINT " + m_name + "_pkey;";
  else if (dbType == Sqlite3)
    return "";

  return "";
}

const QString MyMoneyDbTable::modifyColumnString(databaseTypeE dbType, const QString& columnName, const MyMoneyDbColumn& newDef) const {
  QString qs = "ALTER TABLE " + m_name + " ";
  if (dbType == Mysql)
    qs += "CHANGE " + columnName + " " + newDef.generateDDL(dbType);
  else if (dbType == Postgresql)
    qs += "ALTER COLUMN " + columnName + " TYPE " + newDef.generateDDL(dbType).section(' ', 1);
  else if (dbType == Sqlite3)
    qs = "";
  else if (dbType == Oracle8)
    qs = "MODIFY " + columnName + " " + newDef.generateDDL(dbType);

  return qs;
}

//*****************************************************************************
const QString MyMoneyDbIndex::generateDDL (databaseTypeE dbType) const
{
  Q_UNUSED(dbType);

  QString qs = "CREATE ";

  if (m_unique)
    qs += "UNIQUE ";

  qs += "INDEX " + m_table + "_" + m_name + "_idx ON "
       + m_table + " (";

  // The following should probably be revised.  MySQL supports an index on
  // partial columns, but not on a function.  Postgres supports an index on
  // the result of an SQL function, but not a partial column.  There should be
  // a way to merge these, and support other DBMSs like SQLite at the same time.
  // For now, if we just use plain columns, this will work fine.
  for (QStringList::ConstIterator it = m_columns.begin(); it != m_columns.end(); ++it) {
    qs += *it + ",";
  }

  qs = qs.left(qs.length() - 1) + ");\n";

  return qs;
}

//*****************************************************************************
// These are the actual column types.
// TODO: consider changing all the else-if statements to driver classes.
//

MyMoneyDbColumn*         MyMoneyDbColumn::clone () const
{ return (new MyMoneyDbColumn (*this)); }

MyMoneyDbIntColumn*      MyMoneyDbIntColumn::clone () const
{ return (new MyMoneyDbIntColumn (*this)); }

MyMoneyDbDatetimeColumn* MyMoneyDbDatetimeColumn::clone () const
{ return (new MyMoneyDbDatetimeColumn (*this)); }

MyMoneyDbTextColumn* MyMoneyDbTextColumn::clone () const
{ return (new MyMoneyDbTextColumn (*this)); }

const QString MyMoneyDbColumn::generateDDL (databaseTypeE dbType) const
{
  Q_UNUSED(dbType);

  QString qs = name() + " " + type();
  if (isNotNull()) qs += " NOT NULL";
  return qs;
}

const QString MyMoneyDbIntColumn::generateDDL (databaseTypeE dbType) const
{
  QString qs = name() + " ";

  switch (m_type) {
    case MyMoneyDbIntColumn::TINY:
      if (dbType == Mysql || dbType == Sqlite3) {
        qs += "tinyint ";
      } else if (dbType == Postgresql) {
        qs += "int2 ";
      } else if (dbType == Db2) {
        qs += "smallint ";
      } else if (dbType == Oracle8) {
        qs += "number(3) ";
      } else {
        // cross your fingers...
        qs += "smallint ";
      }
      break;
    case MyMoneyDbIntColumn::SMALL:
      if (dbType == Mysql || dbType == Db2 || dbType == Sqlite3) {
        qs += "smallint ";
      } else if (dbType == Postgresql) {
        qs += "int2 ";
      } else if (dbType == Oracle8) {
        qs += "number(5) ";
      } else {
        // cross your fingers...
        qs += "smallint ";
      }
      break;
    case MyMoneyDbIntColumn::MEDIUM:
      if (dbType == Mysql || dbType == Db2) {
        qs += "int ";
      } else if (dbType == Postgresql) {
        qs += "int4 ";
      } else if (dbType == Sqlite3) {
        qs += "integer ";
      } else if (dbType == Oracle8) {
        qs += "number(10) ";
      } else {
        // cross your fingers...
        qs += "int ";
      }
      break;
    case MyMoneyDbIntColumn::BIG:
      if (dbType == Mysql || dbType == Db2 || dbType == Sqlite3) {
        qs += "bigint ";
      } else if (dbType == Postgresql) {
        qs += "int8 ";
      } else if (dbType == Oracle8) {
        qs += "number(20) ";
      } else {
        // cross your fingers...
        qs += "bigint ";
      }
      break;
    default:
      qs += "int ";
      break;
  }

  if ((! m_isSigned) && (dbType == Mysql || dbType == Sqlite3)) {
    qs += "unsigned ";
  }

  if (isNotNull()) qs += " NOT NULL";
  if ((! m_isSigned) && (dbType == Postgresql)) {
    qs += " check(" + name() + " >= 0)";
  }
  return qs;
}

const QString MyMoneyDbTextColumn::generateDDL (databaseTypeE dbType) const
{
  QString qs = name() + " ";

  switch (m_type) {
    case MyMoneyDbTextColumn::TINY:
      if (dbType == Mysql || dbType == Sqlite3) {
        qs += "tinytext ";
      } else if (dbType == Postgresql) {
        qs += "text ";
      } else if (dbType == Db2) {
        qs += "varchar(255) ";
      } else if (dbType == Oracle8) {
        qs += "varchar2(255) ";
      } else {
        // cross your fingers...
        qs += "tinytext ";
      }
      break;
    case MyMoneyDbTextColumn::NORMAL:
      if (dbType == Mysql || dbType == Sqlite3  || dbType == Postgresql) {
        qs += "text ";
      } else if (dbType == Db2) {
        qs += "clob(64K) ";
      } else if (dbType == Oracle8) {
        qs += "clob ";
      } else {
        // cross your fingers...
        qs += "text ";
      }
      break;
    case MyMoneyDbTextColumn::MEDIUM:
      if (dbType == Mysql || dbType == Sqlite3 ) {
        qs += "mediumtext ";
      } else if (dbType == Postgresql) {
        qs += "text ";
      } else if (dbType == Db2) {
        qs += "clob(16M) ";
      } else if (dbType == Oracle8) {
        qs += "clob ";
      } else {
        // cross your fingers...
        qs += "mediumtext ";
      }
      break;
    case MyMoneyDbTextColumn::LONG:
      if (dbType == Mysql || dbType == Sqlite3 ) {
        qs += "longtext ";
      } else if (dbType == Postgresql) {
        qs += "text ";
      } else if (dbType == Db2) {
        qs += "clob(2G) ";
      } else if (dbType == Oracle8) {
        qs += "clob ";
      } else {
        // cross your fingers...
        qs += "longtext ";
      }
      break;
    default:
      if (dbType == Oracle8) {
        qs += "clob ";
      } else {
        qs += "text ";
      }
      break;
  }

  if (isNotNull()) qs += " NOT NULL";

  return qs;
}

const QString MyMoneyDbDatetimeColumn::generateDDL (databaseTypeE dbType) const
{
  QString qs = name() + " ";
  if (dbType == Mysql  || dbType == ODBC3) {
    qs += "datetime ";
  } else if (dbType == Postgresql || dbType == Db2 || dbType == Oracle8 || dbType == Sqlite3 ) {
    qs += "timestamp ";
  } else {
    qs += "";
  }
  if (isNotNull()) qs += " NOT NULL";
  return qs;
}
