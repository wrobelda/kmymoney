/***************************************************************************
                          mymoneyreport.h
                             -------------------
    begin                : Sun July 4 2004
    copyright            : (C) 2004-2005 by Ace Jones
    email                : acejones@users.sourceforge.net
                           (C) 2017 by Łukasz Wojniłowicz <lukasz.wojnilowicz@gmail.com>

 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef MYMONEYREPORT_H
#define MYMONEYREPORT_H

// ----------------------------------------------------------------------------
// QT Includes

#include <QHashFunctions>

// ----------------------------------------------------------------------------
// Project Includes
#include "mymoneyobject.h"
#include "mymoneytransactionfilter.h"
#include "kmm_mymoney_export.h"
#include "mymoneyunittestable.h"

class QString;
class QDomElement;
class QDomDocument;
class MyMoneyAccount;

template <typename T> class QList;

namespace eMyMoney { namespace Account { enum class Type; }
                     namespace TransactionFilter { enum class Date; } }

/**
  * This class defines a report within the MyMoneyEngine.  The report class
  * contains all the configuration parameters needed to run a report, plus
  * XML serialization.
  *
  * A report is a transactionfilter, so any report can specify which
  * transactions it's interested down to the most minute level of detail.
  * It extends the transactionfilter by providing identification (name,
  * comments, group type, etc) as well as layout information (what kind
  * of layout should be used, how the rows & columns should be presented,
  * currency converted, etc.)
  *
  * As noted above, this class only provides a report DEFINITION.  The
  * generation and presentation of the report itself are left to higher
  * level classes.
  *
  * @author Ace Jones <acejones@users.sourceforge.net>
  */

class MyMoneyReportPrivate;
class KMM_MYMONEY_EXPORT MyMoneyReport: public MyMoneyObject, public MyMoneyTransactionFilter
{
  Q_DECLARE_PRIVATE_D(MyMoneyObject::d_ptr, MyMoneyReport)

  KMM_MYMONEY_UNIT_TESTABLE

//protected:
//    MyMoneyReport(MyMoneyReportPrivate &dd);

public:
  // When adding a new row type, be sure to add a corresponding entry in kTypeArray
  enum ERowType { eNoRows = 0, eAssetLiability, eExpenseIncome, eCategory, eTopCategory, eAccount, eTag, ePayee, eMonth, eWeek, eTopAccount, eAccountByTopAccount, eEquityType, eAccountType, eInstitution, eBudget, eBudgetActual, eSchedule, eAccountInfo, eAccountLoanInfo, eAccountReconcile, eCashFlow};
  enum EReportType { eNoReport = 0, ePivotTable, eQueryTable, eInfoTable };
  enum EColumnType { eNoColumns = 0, eDays = 1, eMonths = 1, eBiMonths = 2, eQuarters = 3, eWeeks = 7, eYears = 12 };

  // if you add bits to this bitmask, start with the value currently assigned to eQCend and update its value afterwards
  // also don't forget to add column names to kQueryColumnsText in mymoneyreport.cpp
  enum EQueryColumns { eQCnone = 0x0, eQCbegin = 0x1, eQCnumber = 0x1, eQCpayee = 0x2, eQCcategory = 0x4, eQCtag = 0x8, eQCmemo = 0x10, eQCaccount = 0x20, eQCreconciled = 0x40, eQCaction = 0x80, eQCshares = 0x100, eQCprice = 0x200, eQCperformance = 0x400, eQCloan = 0x800, eQCbalance = 0x1000, eQCcapitalgain = 0x2000, eQCend = 0x4000 };

  enum EDetailLevel { eDetailNone = 0, eDetailAll, eDetailTop, eDetailGroup, eDetailTotal, eDetailEnd };
  enum EInvestmentSum { eSumPeriod = 0, eSumOwnedAndSold, eSumOwned, eSumSold, eSumBought};
  enum EChartType { eChartNone = 0, eChartLine, eChartBar, eChartPie, eChartRing, eChartStackedBar, eChartEnd };

  enum dataOptionE { automatic = 0, userDefined, dataOptionCount };

  static const QStringList kRowTypeText;
  static const QStringList kColumnTypeText;
  static const QStringList kQueryColumnsText;
  static const QStringList kDetailLevelText;
  static const QStringList kChartTypeText;
  static const EReportType kTypeArray[];

public:
  MyMoneyReport();
  explicit MyMoneyReport(ERowType rt,
                         unsigned ct,
                         eMyMoney::TransactionFilter::Date dl,
                         EDetailLevel ss,
                         const QString& name,
                         const QString& comment);
  /**
    * This constructor creates an object based on the data found in the
    * QDomElement referenced by @p node. If problems arise, the @p id of
    * the object is cleared (see MyMoneyObject::clearId()).
    */
  explicit MyMoneyReport(const QDomElement& node);

  MyMoneyReport(const QString& id,
                const MyMoneyReport& other);

  MyMoneyReport(const MyMoneyReport & other);
  MyMoneyReport(MyMoneyReport && other);
  MyMoneyReport & operator=(MyMoneyReport other);
  friend void swap(MyMoneyReport& first, MyMoneyReport& second);

  ~MyMoneyReport();

  EReportType reportType() const;

  QString name() const;
  void setName(const QString& s);

  bool isShowingRowTotals() const;
  void setShowingRowTotals(bool f);

  bool isShowingColumnTotals() const;
  void setShowingColumnTotals(bool f);

  ERowType rowType() const;
  void setRowType(ERowType rt);
  bool isRunningSum() const;

  EColumnType columnType() const;
  void setColumnType(EColumnType ct);

  bool isConvertCurrency() const;
  void setConvertCurrency(bool f);
  uint columnPitch() const;

  QString comment() const;
  void setComment(const QString& comment);

  EQueryColumns queryColumns() const;
  void setQueryColumns(EQueryColumns qc);

  QString group() const;
  void setGroup(const QString& group);

  bool isFavorite() const;
  void setFavorite(bool f);

  bool isTax() const;
  void setTax(bool f);

  bool isInvestmentsOnly() const;
  void setInvestmentsOnly(bool f);

  bool isLoansOnly() const;
  void setLoansOnly(bool f);

  EDetailLevel detailLevel() const;
  void setDetailLevel(EDetailLevel detail);

  EInvestmentSum investmentSum() const;
  void setInvestmentSum(EInvestmentSum sum);

  bool isHideTransactions() const;
  void setHideTransactions(bool f);

  EChartType chartType() const;
  void setChartType(EChartType type);

  bool isChartDataLabels() const;
  void setChartDataLabels(bool f);

  bool isChartCHGridLines() const;
  void setChartCHGridLines(bool f);

  bool isChartSVGridLines() const;
  void setChartSVGridLines(bool f);

  bool isChartByDefault() const;
  void setChartByDefault(bool f);

  uint chartLineWidth() const;
  void setChartLineWidth(uint f);

  bool isLogYAxis() const;
  void setLogYAxis(bool f);

  QString dataRangeStart() const;
  void setDataRangeStart(const QString& f);

  QString dataRangeEnd() const;
  void setDataRangeEnd(const QString& f);

  QString dataMajorTick() const;
  void setDataMajorTick(const QString& f);

  QString dataMinorTick() const;
  void setDataMinorTick(const QString& f);

  uint yLabelsPrecision() const;
  void setYLabelsPrecision(int f);

  bool isIncludingSchedules() const;
  void setIncludingSchedules(bool f);

  bool isColumnsAreDays() const;
  void setColumnsAreDays(bool f);

  bool isIncludingTransfers() const;
  void setIncludingTransfers(bool f);

  bool isIncludingUnusedAccounts() const;
  void setIncludingUnusedAccounts(bool f);

  bool hasBudget() const;
  QString budget() const;

  /**
    * Sets the budget used for this report
    *
    * @param budget The ID of the budget to use, or an empty string
    * to indicate a budget is NOT included
    * @param fa Whether to display actual data alongside the budget.
    * Setting to false means the report displays ONLY the budget itself.
    * @warning For now, the budget ID is ignored.  The budget id is
    * simply checked for any non-empty string, and if so, hasBudget()
    * will return true.
    */
  void setBudget(const QString& budget, bool fa = true);

  bool isIncludingBudgetActuals() const;
  void setIncludingBudgetActuals(bool f);

  bool isIncludingForecast() const;
  void setIncludingForecast(bool f);

  bool isIncludingMovingAverage() const;
  void setIncludingMovingAverage(bool f);

  int movingAverageDays() const;
  void setMovingAverageDays(int days);

  bool isIncludingPrice() const;
  void setIncludingPrice(bool f);

  bool isIncludingAveragePrice() const;
  void setIncludingAveragePrice(bool f);

  dataOptionE dataFilter() const;
  bool isDataUserDefined() const;
  void setDataFilter(dataOptionE u);

  eMyMoney::TransactionFilter::Date dateRange() const;
  bool isDateUserDefined() const;

  /**
    * Set the underlying date filter and LOCK that filter to the specified
    * range.  For example, if @p _u is "CurrentMonth", this report should always
    * be updated to the current month no matter when the report is run.
    *
    * This updating is not entirely automatic, you should update it yourself by
    * calling updateDateFilter.
    *
    * @param _u The date range constant (MyMoneyTransactionFilter::dateRangeE)
    *          which this report should be locked to.
    */

  void setDateFilter(eMyMoney::TransactionFilter::Date u);

  /**
    * Set the underlying date filter using the start and end dates provided.
    * Note that this does not LOCK to any range like setDateFilter(unsigned)
    * above.  It is just a reimplementation of the MyMoneyTransactionFilter
    * version.
    *
    * @param _db The inclusive begin date of the date range
    * @param _de The inclusive end date of the date range
    */

  void setDateFilter(const QDate& db, const QDate& de);

  /**
    * Set the underlying date filter using the 'date lock' property.
    *
    * Always call this function before executing the report to be sure that
    * the date filters properly match the plain-language 'date lock'.
    *
    * For example, if the report is date-locked to "Current Month", and the
    * last time you loaded or ran the report was in August, but it's now
    * September, this function will update the date range to be September,
    * as is proper.
    */
  void updateDateFilter();

  bool isMixedTime() const;
  void setMixedTime(bool f);

  int currentDateColumn() const;
  void setCurrentDateColumn(int f);

  uint settlementPeriod() const;
  void setSettlementPeriod(uint days);

  bool isShowingSTLTCapitalGains() const;
  void setShowSTLTCapitalGains(bool f);

  QDate termSeparator() const;
  void setTermSeparator(const QDate& date);

  bool isSkippingZero() const;
  void setSkipZero(int f);

  /**
    * This method allows you to clear the underlying transaction filter
    */
  void clearTransactionFilter();

  /**
    * This method allows you to set the underlying transaction filter
    *
    * @param _filter The filter which should replace the existing transaction
    * filter.
    */
  void assignFilter(const MyMoneyTransactionFilter& filter);

  /**
    * Retrieves a VALID beginning & ending date for this report.
    *
    * The underlying date filter can return en empty QDate() for either the
    * begin or end date or both.  This is typically unacceptable for reports,
    * which need the REAL begin and end date.
    *
    * This function gets the underlying date filter range, and if either is
    * an empty QDate(), it determines the missing date from looking at all
    * the transactions which match the underlying filter, and returning the
    * date of the first or last transaction (as appropriate).
    *
    * @param _db The inclusive begin date of the date range
    * @param _de The inclusive end date of the date range
    */
  void validDateRange(QDate &db, QDate &de);

  /**
    * This method turns on the account group filter and adds the
    * @p type to the list of allowed groups.
    *
    * Note that account group filtering is handled differently
    * than all the filters of the underlying class.  This filter
    * is meant to be applied to individual splits of matched
    * transactions AFTER the underlying filter is used to find
    * the matching transactions.
    *
    * @param type the account group to add to the allowed groups list
    */
  void addAccountGroup(eMyMoney::Account::Type type);

  /**
    * This method returns whether an account group filter has been set,
    * and if so, it returns all the account groups set in the filter.
    *
    * @param list list to append account groups into
    * @return return true if an account group filter has been set
    */
  bool accountGroups(QList<eMyMoney::Account::Type>& list) const;

  /**
    * This method returns whether the specified account group
    * is allowed by the account groups filter.
    *
    * @param type group to append account groups into
    * @return return true if an account group filter has been set
    */
  bool includesAccountGroup(eMyMoney::Account::Type type) const;

  /**
    * This method is used to test whether a specific account
    * passes the accountGroup test and either the Account or
    * Category test, depending on which sort of Account it is.
    *
    * The m_tax and m_investments properties are also considered.
    *
    * @param acc the account in question
    * @return true if account is in filter set, false otherwise
    */
  bool includes(const MyMoneyAccount& acc) const;

  /**
    * This method writes this report to the DOM element @p e,
    * within the DOM document @p doc.
    *
    * @param e The element which should be populated with info from this report
    * @param doc The document which we can use to create new sub-elements
    *              if needed
    * @param anonymous Whether the sensitive parts of the report should be
    *              masked
    */
  void write(QDomElement& e, QDomDocument *doc, bool anonymous = false) const;

  /**
    * This method reads a report from the DOM element @p e, and
    * populates this report with the results.
    *
    * @param e The element from which the report should be read
    *
    * @return bool True if a report was successfully loaded from the
    *    element @p e.  If false is returned, the contents of this report
    *    object are undefined.
    */
  bool read(const QDomElement& e);

  /**
    * This method creates a QDomElement for the @p document
    * under the parent node @p parent.  (This version overwrites the
    * MMObject base class.)
    *
    * @param document reference to QDomDocument
    * @param parent reference to QDomElement parent node
    */
  void writeXML(QDomDocument& document, QDomElement& parent) const override;

  /**
    * This method checks if a reference to the given object exists. It returns,
    * a @p true if the object is referencing the one requested by the
    * parameter @p id. If it does not, this method returns @p false.
    *
    * @param id id of the object to be checked for references
    * @retval true This object references object with id @p id.
    * @retval false This object does not reference the object with id @p id.
    */
  bool hasReferenceTo(const QString& id) const override;

  /**
    * This method allows to modify the default lineWidth for graphs.
    * The default is 2.
    */
  static void setLineWidth(int width);

  /**
    * This member keeps the current setting for line graphs lineWidth.
    * @sa setLineWidth()
    */
  static int m_lineWidth;

  /**
   * Return row type as string.
   *
   * @param type type to get string for
   * @return row type converted to string
   */
  static QString toString(ERowType type);

  /**
   * Return report type as string.
   *
   * @param type report type to get string for
   * @return report type converted to string
   */
  static QString toString(EReportType type);
};

inline void swap(MyMoneyReport& first, MyMoneyReport& second) // krazy:exclude=inline
{
  using std::swap;
  swap(first.MyMoneyObject::d_ptr, second.MyMoneyObject::d_ptr);
  swap(first.MyMoneyTransactionFilter::d_ptr, second.MyMoneyTransactionFilter::d_ptr);
}

inline MyMoneyReport::MyMoneyReport(MyMoneyReport && other) : MyMoneyReport() // krazy:exclude=inline
{
  swap(*this, other);
}

inline MyMoneyReport & MyMoneyReport::operator=(MyMoneyReport other) // krazy:exclude=inline
{
  swap(*this, other);
  return *this;
}

/**
  * Make it possible to hold @ref MyMoneyReport objects inside @ref QVariant objects.
  */
Q_DECLARE_METATYPE(MyMoneyReport)

#endif // MYMONEYREPORT_H
