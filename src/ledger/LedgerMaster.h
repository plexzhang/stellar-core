#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <string>
#include "ledger/LedgerGateway.h"
#include "ledger/LedgerHeaderFrame.h"

/*
Holds the current ledger
Applies the tx set to the last ledger to get the next one
Hands the old ledger off to the history
*/

namespace medida { class Timer; }

namespace stellar
{
    class Application;
    class Database;
    class LedgerDelta;

    class LedgerMaster : public LedgerGateway
    {
        bool mCaughtUp;

        LedgerHeaderFrame::pointer mLastClosedLedger;
        LedgerHeaderFrame::pointer mCurrentLedger;

        Application &mApp;
        medida::Timer& mTransactionApply;
        medida::Timer& mLedgerClose;

        void startCatchUp();
        
        // called on startup to get the last CLF we knew about
        void syncWithCLF();

    public:

        typedef std::shared_ptr<LedgerMaster>           pointer;
        typedef const std::shared_ptr<LedgerMaster>&    ref;

        LedgerMaster(Application& app);

        //////// GATEWAY FUNCTIONS
        // called by txherder
        void externalizeValue(TxSetFramePtr txSet, uint64_t closeTime, int32_t baseFee);

        uint64_t getLedgerNum();
        int64_t getMinBalance(uint32_t ownerCount);
        int32_t getTxFee();
        uint64_t getCloseTime();

        ///////

        void startNewLedger();
        void loadLastKnownLedger();

        // establishes that our internal representation is in sync with passed ledger
        //bool ensureSync(Ledger::pointer lastClosedLedger);

        // called before starting to make changes to the db
        void beginClosingLedger();
        // called every time we successfully closed a ledger
        //bool commitLedgerClose(Ledger::pointer ledger);
        // called when we could not close the ledger
        void abortLedgerClose();

        LedgerHeader& getCurrentLedgerHeader();
        LedgerHeader& getLastClosedLedgerHeader();

        Database& getDatabase();

		void closeLedger(TxSetFramePtr txSet, uint64_t closeTime, int32_t baseFee);

        // state store
        enum StoreStateName {
            kLastClosedLedger = 0,
            kLastEntry
        };

        std::string getState(StoreStateName stateName);
        void setState(StoreStateName stateName, const std::string &value);

        static void dropAll(Database &db);
    private:
        std::string getStoreStateName(StoreStateName n);
        void closeLedgerHelper(bool updateCurrent, LedgerDelta const& delta);

        static const char *kSQLCreateStatement;

    };
}






