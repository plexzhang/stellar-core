// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupWork.h"
#include "catchup/CatchupWorkTests.h"
#include "crypto/Hex.h"
#include "herder/LedgerCloseData.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/GunzipFileWork.h"
#include "historywork/GzipFileWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "ledger/CheckpointRange.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ExternalQueue.h"
#include "main/PersistentState.h"
#include "process/ProcessManager.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/NonCopyable.h"
#include "util/Timer.h"
#include "util/TmpDir.h"
#include "work/WorkManager.h"
#include "work/WorkParent.h"

#include <cstdio>
#include <fstream>
#include <lib/util/format.h>
#include <medida/counter.h>
#include <medida/metrics_registry.h>
#include <random>
#include <xdrpp/autocheck.h>

using namespace stellar;

namespace stellar
{
using namespace txtest;
using xdr::operator==;
};

class Configurator : NonCopyable
{
  public:
    virtual Config& configure(Config& cfg, bool writable) const = 0;
    virtual std::string
    getArchiveDirName() const
    {
        return "";
    }
};

class TmpDirConfigurator : public Configurator
{
    TmpDirManager mArchtmp;
    TmpDir mDir;

  public:
    TmpDirConfigurator() : mArchtmp("archtmp"), mDir(mArchtmp.tmpDir("archive"))
    {
    }

    std::string
    getArchiveDirName() const override
    {
        return mDir.getName();
    }

    Config&
    configure(Config& cfg, bool writable) const override
    {
        std::string d = mDir.getName();
        std::string getCmd = "cp " + d + "/{0} {1}";
        std::string putCmd = "";
        std::string mkdirCmd = "";

        if (writable)
        {
            putCmd = "cp {0} " + d + "/{1}";
            mkdirCmd = "mkdir -p " + d + "/{0}";
        }

        cfg.HISTORY["test"] =
            std::make_shared<HistoryArchive>("test", getCmd, putCmd, mkdirCmd);
        return cfg;
    }
};

struct CatchupMetrics
{
    uint32_t mHistoryArchiveStatesDownloaded;
    uint32_t mLedgersDownloaded;
    uint32_t mLedgersVerified;
    uint32_t mLedgerChainsVerificationFailed;
    uint32_t mBucketsDownloaded;
    uint32_t mBucketsApplied;
    uint32_t mTransactionsDownloaded;
    uint32_t mTransactionsApplied;

    CatchupMetrics()
        : mHistoryArchiveStatesDownloaded{0}
        , mLedgersDownloaded{0}
        , mLedgersVerified{0}
        , mLedgerChainsVerificationFailed{0}
        , mBucketsDownloaded{false}
        , mBucketsApplied{false}
        , mTransactionsDownloaded{0}
        , mTransactionsApplied{0}
    {
    }

    CatchupMetrics(uint32_t historyArchiveStatesDownloaded,
                   uint32_t ledgersDownloaded, uint32_t ledgersVerified,
                   uint32_t ledgerChainsVerificationFailed,
                   uint32_t bucketsDownloaded, uint32_t bucketsApplied,
                   uint32_t transactionsDownloaded,
                   uint32_t transactionsApplied)
        : mHistoryArchiveStatesDownloaded{historyArchiveStatesDownloaded}
        , mLedgersDownloaded{ledgersDownloaded}
        , mLedgersVerified{ledgersVerified}
        , mLedgerChainsVerificationFailed{ledgerChainsVerificationFailed}
        , mBucketsDownloaded{bucketsDownloaded}
        , mBucketsApplied{bucketsApplied}
        , mTransactionsDownloaded{transactionsDownloaded}
        , mTransactionsApplied{transactionsApplied}
    {
    }

    friend CatchupMetrics
    operator-(CatchupMetrics const& x, CatchupMetrics const& y)
    {
        return CatchupMetrics{x.mHistoryArchiveStatesDownloaded -
                                  y.mHistoryArchiveStatesDownloaded,
                              x.mLedgersDownloaded - y.mLedgersDownloaded,
                              x.mLedgersVerified - y.mLedgersVerified,
                              x.mLedgerChainsVerificationFailed -
                                  y.mLedgerChainsVerificationFailed,
                              x.mBucketsDownloaded - y.mBucketsDownloaded,
                              x.mBucketsApplied - y.mBucketsApplied,
                              x.mTransactionsDownloaded -
                                  y.mTransactionsDownloaded,
                              x.mTransactionsApplied - y.mTransactionsApplied};
    }
};

struct CatchupPerformedWork
{
    uint32_t mHistoryArchiveStatesDownloaded;
    uint32_t mLedgersDownloaded;
    uint32_t mLedgersVerified;
    uint32_t mLedgerChainsVerificationFailed;
    bool mBucketsDownloaded;
    bool mBucketsApplied;
    uint32_t mTransactionsDownloaded;
    uint32_t mTransactionsApplied;

    CatchupPerformedWork(CatchupMetrics const& metrics)
        : mHistoryArchiveStatesDownloaded{metrics
                                              .mHistoryArchiveStatesDownloaded}
        , mLedgersDownloaded{metrics.mLedgersDownloaded}
        , mLedgersVerified{metrics.mLedgersVerified}
        , mLedgerChainsVerificationFailed{metrics
                                              .mLedgerChainsVerificationFailed}
        , mBucketsDownloaded{metrics.mBucketsDownloaded > 0}
        , mBucketsApplied{metrics.mBucketsApplied > 0}
        , mTransactionsDownloaded{metrics.mTransactionsDownloaded}
        , mTransactionsApplied{metrics.mTransactionsApplied}
    {
    }

    CatchupPerformedWork(uint32_t historyArchiveStatesDownloaded,
                         uint32_t ledgersDownloaded, uint32_t ledgersVerified,
                         uint32_t ledgerChainsVerificationFailed,
                         bool bucketsDownloaded, bool bucketsApplied,
                         uint32_t transactionsDownloaded,
                         uint32_t transactionsApplied)
        : mHistoryArchiveStatesDownloaded{historyArchiveStatesDownloaded}
        , mLedgersDownloaded{ledgersDownloaded}
        , mLedgersVerified{ledgersVerified}
        , mLedgerChainsVerificationFailed{ledgerChainsVerificationFailed}
        , mBucketsDownloaded{bucketsDownloaded}
        , mBucketsApplied{bucketsApplied}
        , mTransactionsDownloaded{transactionsDownloaded}
        , mTransactionsApplied{transactionsApplied}
    {
    }

    friend bool
    operator==(CatchupPerformedWork const& x, CatchupPerformedWork const& y)
    {
        if (x.mHistoryArchiveStatesDownloaded !=
            y.mHistoryArchiveStatesDownloaded)
        {
            return false;
        }
        if (x.mLedgersDownloaded != y.mLedgersDownloaded)
        {
            return false;
        }
        if (x.mLedgersVerified != y.mLedgersVerified)
        {
            return false;
        }
        if (x.mLedgerChainsVerificationFailed !=
            y.mLedgerChainsVerificationFailed)
        {
            return false;
        }
        if (x.mBucketsDownloaded != y.mBucketsDownloaded)
        {
            return false;
        }
        if (x.mBucketsApplied != y.mBucketsApplied)
        {
            return false;
        }
        if (x.mTransactionsDownloaded != y.mTransactionsDownloaded)
        {
            return false;
        }
        if (x.mTransactionsApplied != y.mTransactionsApplied)
        {
            return false;
        }
        return true;
    }
    friend bool
    operator!=(CatchupPerformedWork const& x, CatchupPerformedWork const& y)
    {
        return !(x == y);
    }
};

namespace Catch
{
template <>
std::string
toString(CatchupPerformedWork const& cm)
{
    return fmt::format("{}, {}, {}, {}, {}, {}, {}, {}",
                       cm.mHistoryArchiveStatesDownloaded,
                       cm.mLedgersDownloaded, cm.mLedgersVerified,
                       cm.mLedgerChainsVerificationFailed,
                       cm.mBucketsDownloaded, cm.mBucketsApplied,
                       cm.mTransactionsDownloaded, cm.mTransactionsApplied);
}
}

class HistoryTests
{
  protected:
    VirtualClock clock;
    std::shared_ptr<Configurator> mConfigurator;
    Config cfg;
    std::vector<Config> mCfgs;
    Application::pointer appPtr;
    Application& app;

    std::default_random_engine mGenerator;
    std::bernoulli_distribution mFlip{0.5};

    std::vector<LedgerCloseData> mLedgerCloseDatas;

    std::vector<uint32_t> mLedgerSeqs;
    std::vector<uint256> mLedgerHashes;
    std::vector<uint256> mBucketListHashes;
    std::vector<uint256> mBucket0Hashes;
    std::vector<uint256> mBucket1Hashes;

    std::vector<int64_t> rootBalances;
    std::vector<int64_t> aliceBalances;
    std::vector<int64_t> bobBalances;
    std::vector<int64_t> carolBalances;

    std::vector<SequenceNumber> rootSeqs;
    std::vector<SequenceNumber> aliceSeqs;
    std::vector<SequenceNumber> bobSeqs;
    std::vector<SequenceNumber> carolSeqs;

  public:
    HistoryTests(std::shared_ptr<Configurator> cg =
                     std::make_shared<TmpDirConfigurator>())
        : mConfigurator(cg)
        , cfg(getTestConfig())
        , appPtr(
              Application::create(clock, mConfigurator->configure(cfg, true)))
        , app(*appPtr)
    {
        CHECK(HistoryManager::initializeHistoryArchive(app, "test"));
    }

    void crankTillDone();
    void generateRandomLedger();
    void generateAndPublishHistory(size_t nPublishes);
    void generateAndPublishInitialHistory(size_t nPublishes);

    Application::pointer catchupNewApplication(uint32_t initLedger,
                                               uint32_t count, bool manual,
                                               Config::TestDbMode dbMode,
                                               std::string const& appName);

    bool catchupApplication(uint32_t initLedger, uint32_t count, bool manual,
                            Application::pointer app2, bool doStart = true,
                            uint32_t gap = 0);

    CatchupMetrics getCatchupMetrics(Application::pointer app);
    CatchupPerformedWork computeCatchupPerformedWork(
        uint32_t lastClosedLedger,
        CatchupConfiguration const& catchupConfiguration,
        HistoryManager const& historyManager);

    bool
    flip()
    {
        return mFlip(mGenerator);
    }
};

void
HistoryTests::crankTillDone()
{
    while (!app.getWorkManager().allChildrenDone() &&
           !app.getClock().getIOService().stopped())
    {
        app.getClock().crank(true);
    }
}

void
HistoryTests::generateAndPublishInitialHistory(size_t nPublishes)
{
    app.start();

    auto& lm = app.getLedgerManager();

    // At this point LCL should be 1, current ledger should be 2
    assert(lm.getLastClosedLedgerHeader().header.ledgerSeq == 1);
    assert(lm.getCurrentLedgerHeader().ledgerSeq == 2);

    generateAndPublishHistory(nPublishes);
}

TEST_CASE_METHOD(HistoryTests, "next checkpoint ledger", "[history]")
{
    HistoryManager& hm = app.getHistoryManager();
    CHECK(hm.nextCheckpointLedger(0) == 64);
    CHECK(hm.nextCheckpointLedger(1) == 64);
    CHECK(hm.nextCheckpointLedger(32) == 64);
    CHECK(hm.nextCheckpointLedger(62) == 64);
    CHECK(hm.nextCheckpointLedger(63) == 64);
    CHECK(hm.nextCheckpointLedger(64) == 64);
    CHECK(hm.nextCheckpointLedger(65) == 128);
    CHECK(hm.nextCheckpointLedger(66) == 128);
    CHECK(hm.nextCheckpointLedger(126) == 128);
    CHECK(hm.nextCheckpointLedger(127) == 128);
    CHECK(hm.nextCheckpointLedger(128) == 128);
    CHECK(hm.nextCheckpointLedger(129) == 192);
    CHECK(hm.nextCheckpointLedger(130) == 192);
}

TEST_CASE_METHOD(HistoryTests, "HistoryManager::compress", "[history]")
{
    std::string s = "hello there";
    HistoryManager& hm = app.getHistoryManager();
    std::string fname = hm.localFilename("compressme");
    {
        std::ofstream out(fname, std::ofstream::binary);
        out.write(s.data(), s.size());
    }
    std::string compressed = fname + ".gz";
    auto& wm = app.getWorkManager();
    auto g = wm.addWork<GzipFileWork>(fname);
    wm.advanceChildren();
    crankTillDone();
    REQUIRE(g->getState() == Work::WORK_SUCCESS);
    REQUIRE(!fs::exists(fname));
    REQUIRE(fs::exists(compressed));

    auto u = wm.addWork<GunzipFileWork>(compressed);
    wm.advanceChildren();
    crankTillDone();
    REQUIRE(u->getState() == Work::WORK_SUCCESS);
    REQUIRE(fs::exists(fname));
    REQUIRE(!fs::exists(compressed));
}

TEST_CASE_METHOD(HistoryTests, "HistoryArchiveState::get_put", "[history]")
{
    HistoryArchiveState has;
    has.currentLedger = 0x1234;

    auto i = app.getConfig().HISTORY.find("test");
    REQUIRE(i != app.getConfig().HISTORY.end());
    auto archive = i->second;

    has.resolveAllFutures();

    auto& wm = app.getWorkManager();
    auto put = wm.addWork<PutHistoryArchiveStateWork>(has, archive);
    wm.advanceChildren();
    crankTillDone();
    REQUIRE(put->getState() == Work::WORK_SUCCESS);

    HistoryArchiveState has2;
    auto get = wm.addWork<GetHistoryArchiveStateWork>(
        "get-history-archive-state", has2, 0, std::chrono::seconds(0), archive);
    wm.advanceChildren();
    crankTillDone();
    REQUIRE(get->getState() == Work::WORK_SUCCESS);
    REQUIRE(has2.currentLedger == 0x1234);
}

void
HistoryTests::generateRandomLedger()
{
    auto& lm = app.getLedgerManager();
    TxSetFramePtr txSet =
        std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);

    uint32_t ledgerSeq = lm.getLedgerNum();
    uint64_t minBalance = lm.getMinBalance(5);
    uint64_t big = minBalance + ledgerSeq;
    uint64_t small = 100 + ledgerSeq;
    uint64_t closeTime = 60 * 5 * ledgerSeq;

    auto root = TestAccount{app, getRoot(app.getNetworkID())};
    auto alice = TestAccount{app, getAccount("alice")};
    auto bob = TestAccount{app, getAccount("bob")};
    auto carol = TestAccount{app, getAccount("carol")};

    // Root sends to alice every tx, bob every other tx, carol every 4rd tx.
    txSet->add(root.tx({createAccount(alice, big)}));
    txSet->add(root.tx({createAccount(bob, big)}));
    txSet->add(root.tx({createAccount(carol, big)}));
    txSet->add(root.tx({payment(alice, big)}));
    txSet->add(root.tx({payment(bob, big)}));
    txSet->add(root.tx({payment(carol, big)}));

    // They all randomly send a little to one another every ledger after #4
    if (ledgerSeq > 4)
    {
        if (flip())
            txSet->add(alice.tx({payment(bob, small)}));
        if (flip())
            txSet->add(alice.tx({payment(carol, small)}));

        if (flip())
            txSet->add(bob.tx({payment(alice, small)}));
        if (flip())
            txSet->add(bob.tx({payment(carol, small)}));

        if (flip())
            txSet->add(carol.tx({payment(alice, small)}));
        if (flip())
            txSet->add(carol.tx({payment(bob, small)}));
    }

    // Provoke sortForHash and hash-caching:
    txSet->getContentsHash();

    CLOG(DEBUG, "History") << "Closing synthetic ledger " << ledgerSeq
                           << " with " << txSet->size() << " txs (txhash:"
                           << hexAbbrev(txSet->getContentsHash()) << ")";

    StellarValue sv(txSet->getContentsHash(), closeTime, emptyUpgradeSteps, 0);
    mLedgerCloseDatas.emplace_back(ledgerSeq, txSet, sv);
    lm.closeLedger(mLedgerCloseDatas.back());

    mLedgerSeqs.push_back(lm.getLastClosedLedgerHeader().header.ledgerSeq);
    mLedgerHashes.push_back(lm.getLastClosedLedgerHeader().hash);
    mBucketListHashes.push_back(
        lm.getLastClosedLedgerHeader().header.bucketListHash);
    mBucket0Hashes.push_back(app.getBucketManager()
                                 .getBucketList()
                                 .getLevel(0)
                                 .getCurr()
                                 ->getHash());
    mBucket1Hashes.push_back(app.getBucketManager()
                                 .getBucketList()
                                 .getLevel(2)
                                 .getCurr()
                                 ->getHash());

    rootBalances.push_back(root.getBalance());
    aliceBalances.push_back(alice.getBalance());
    bobBalances.push_back(bob.getBalance());
    carolBalances.push_back(carol.getBalance());

    rootSeqs.push_back(root.loadSequenceNumber());
    aliceSeqs.push_back(alice.loadSequenceNumber());
    bobSeqs.push_back(bob.loadSequenceNumber());
    carolSeqs.push_back(carol.loadSequenceNumber());
}

void
HistoryTests::generateAndPublishHistory(size_t nPublishes)
{
    auto& lm = app.getLedgerManager();
    auto& hm = app.getHistoryManager();

    size_t publishSuccesses = hm.getPublishSuccessCount();
    SequenceNumber ledgerSeq = lm.getCurrentLedgerHeader().ledgerSeq;

    while (hm.getPublishSuccessCount() < (publishSuccesses + nPublishes))
    {
        uint64_t queueCount = hm.getPublishQueueCount();
        while (hm.getPublishQueueCount() == queueCount)
        {
            generateRandomLedger();
            ++ledgerSeq;
        }

        REQUIRE(lm.getCurrentLedgerHeader().ledgerSeq == ledgerSeq);

        // Advance until we've published (or failed to!)
        while (hm.getPublishSuccessCount() < hm.getPublishQueueCount())
        {
            REQUIRE(hm.getPublishFailureCount() == 0);
            app.getClock().crank(true);
        }
    }

    REQUIRE(hm.getPublishFailureCount() == 0);
    REQUIRE(hm.getPublishSuccessCount() == publishSuccesses + nPublishes);
    REQUIRE(lm.getLedgerNum() ==
            ((publishSuccesses + nPublishes) * hm.getCheckpointFrequency()));
}

Application::pointer
HistoryTests::catchupNewApplication(uint32_t initLedger, uint32_t count,
                                    bool manual, Config::TestDbMode dbMode,
                                    std::string const& appName)
{

    CLOG(INFO, "History") << "****";
    CLOG(INFO, "History") << "**** Beginning catchup test for app '" << appName
                          << "'";
    CLOG(INFO, "History") << "****";

    mCfgs.emplace_back(
        getTestConfig(static_cast<int>(mCfgs.size()) + 1, dbMode));
    mCfgs.back().CATCHUP_COMPLETE =
        count == std::numeric_limits<uint32_t>::max();
    if (count != std::numeric_limits<uint32_t>::max())
    {
        mCfgs.back().CATCHUP_RECENT = count;
    }
    Application::pointer app2 = Application::create(
        clock, mConfigurator->configure(mCfgs.back(), false));

    app2->start();
    CHECK(catchupApplication(initLedger, count, manual, app2) == true);
    return app2;
}

bool
HistoryTests::catchupApplication(uint32_t initLedger, uint32_t count,
                                 bool manual, Application::pointer app2,
                                 bool doStart, uint32_t gap)
{
    auto startCatchupMetrics = getCatchupMetrics(app2);

    auto root = TestAccount{*app2, getRoot(app.getNetworkID())};
    auto alice = TestAccount{*app2, getAccount("alice")};
    auto bob = TestAccount{*app2, getAccount("bob")};
    auto carol = TestAccount{*app2, getAccount("carol")};

    auto& lm = app2->getLedgerManager();
    auto toLedger =
        manual ? initLedger
               : app2->getHistoryManager().nextCheckpointLedger(initLedger) - 1;
    if (doStart)
    {
        // Normally Herder calls LedgerManager.externalizeValue(initLedger) and
        // this _triggers_ catchup within the LM. However, we do this
        // out-of-order because we want to control the catchup mode rather than
        // let the LM pick it, and because we want to simulate a 1-ledger skew
        // between the publishing side and the catchup side so that the catchup
        // has "heard" exactly 1 consensus LedgerCloseData broadcast after the
        // event that triggered its catchup to begin.
        //
        // For example: we want initLedger to be (say) 191-or-less, so that it
        // catches up using block 3, but we want the publisher to advance past
        // 192 (the first entry in block 4) and externalize that value, so that
        // the catchup can see a {192}.prevHash to knit up block 3 against.

        CLOG(INFO, "History") << "force-starting catchup at initLedger="
                              << initLedger;

        lm.startCatchUp({toLedger, count}, manual);
    }

    // Push publishing side forward one-ledger into a history block if it's
    // sitting on the boundary of it. This will ensure there's something
    // externalizable to knit-up with on the catchup side.
    if (app.getHistoryManager().nextCheckpointLedger(
            app.getLedgerManager().getLastClosedLedgerNum()) ==
        app.getLedgerManager().getLedgerNum())
    {
        CLOG(INFO, "History")
            << "force-publishing first ledger in next history block, ledger="
            << app.getLedgerManager().getLedgerNum();
        generateRandomLedger();
    }

    // Externalize (to the catchup LM) the range of ledgers between initLedger
    // and as near as we can get to the first ledger of the block after
    // initLedger (inclusive), so that there's something to knit-up with. Do not
    // externalize anything we haven't yet published, of course.
    if (!manual)
    {
        uint32_t nextBlockStart =
            app.getHistoryManager().nextCheckpointLedger(initLedger);
        // use uint64_t for n to prevent overflows
        for (uint64_t n = initLedger; n <= nextBlockStart; ++n)
        {
            // Remember the vectors count from 2, not 0.
            if (n - 2 >= mLedgerCloseDatas.size())
            {
                break;
            }
            if (n == gap)
            {
                CLOG(INFO, "History")
                    << "simulating LedgerClose transmit gap at ledger " << n;
            }
            else
            {
                // Remember the vectors count from 2, not 0.
                auto const& lcd = mLedgerCloseDatas.at(n - 2);
                CLOG(INFO, "History")
                    << "force-externalizing LedgerCloseData for " << n
                    << " has txhash:"
                    << hexAbbrev(lcd.getTxSet()->getContentsHash());
                lm.valueExternalized(lcd);
            }
        }
    }

    uint32_t lastLedger = lm.getLastClosedLedgerNum();
    auto catchupConfiguration = CatchupConfiguration(toLedger, count);

    assert(!app2->getClock().getIOService().stopped());

    while (!app2->getWorkManager().allChildrenDone())
    {
        app2->getClock().crank(false);
    }

    if (app2->getLedgerManager().getState() != LedgerManager::LM_SYNCED_STATE)
    {
        return false;
    }

    auto endCatchupMetrics = getCatchupMetrics(app2);
    auto catchupPerformedWork =
        CatchupPerformedWork{endCatchupMetrics - startCatchupMetrics};

    REQUIRE(catchupPerformedWork ==
            computeCatchupPerformedWork(lastLedger, catchupConfiguration,
                                        app2->getHistoryManager()));

    uint32_t nextLedger = lm.getLedgerNum();

    CLOG(INFO, "History") << "Caught up: lastLedger = " << lastLedger;
    CLOG(INFO, "History") << "Caught up: initLedger = " << initLedger;
    CLOG(INFO, "History") << "Caught up: nextLedger = " << nextLedger;
    CLOG(INFO, "History") << "Caught up: published range is "
                          << mLedgerSeqs.size() << " ledgers, covering "
                          << "[" << mLedgerSeqs.front() << ", "
                          << mLedgerSeqs.back() << "]";

    // Assuming we caught up to nextLedger 128 (say), LCL will be 127, so we
    // must subtract 1.
    //
    // The local history vectors are built starting from ledger 2 (put at
    // vector-entry 0), so
    // to access slot 127 we must subtract 2 more.
    //
    // So cumulatively: we want to probe local history slot i = nextLedger - 3.

    assert(nextLedger != 0);
    if (nextLedger >= 3)
    {
        size_t i = nextLedger - 3;

        auto wantSeq = mLedgerSeqs.at(i);
        auto wantHash = mLedgerHashes.at(i);
        auto wantBucketListHash = mBucketListHashes.at(i);
        auto wantBucket0Hash = mBucket0Hashes.at(i);
        auto wantBucket1Hash = mBucket1Hashes.at(i);

        auto haveSeq = lm.getLastClosedLedgerHeader().header.ledgerSeq;
        auto haveHash = lm.getLastClosedLedgerHeader().hash;
        auto haveBucketListHash =
            lm.getLastClosedLedgerHeader().header.bucketListHash;
        auto haveBucket0Hash = app2->getBucketManager()
                                   .getBucketList()
                                   .getLevel(0)
                                   .getCurr()
                                   ->getHash();
        auto haveBucket1Hash = app2->getBucketManager()
                                   .getBucketList()
                                   .getLevel(2)
                                   .getCurr()
                                   ->getHash();

        CLOG(INFO, "History") << "Caught up: want Seq[" << i
                              << "] = " << wantSeq;
        CLOG(INFO, "History") << "Caught up: have Seq[" << i
                              << "] = " << haveSeq;

        CLOG(INFO, "History") << "Caught up: want Hash[" << i
                              << "] = " << hexAbbrev(wantHash);
        CLOG(INFO, "History") << "Caught up: have Hash[" << i
                              << "] = " << hexAbbrev(haveHash);

        CLOG(INFO, "History") << "Caught up: want BucketListHash[" << i
                              << "] = " << hexAbbrev(wantBucketListHash);
        CLOG(INFO, "History") << "Caught up: have BucketListHash[" << i
                              << "] = " << hexAbbrev(haveBucketListHash);

        CLOG(INFO, "History") << "Caught up: want Bucket0Hash[" << i
                              << "] = " << hexAbbrev(wantBucket0Hash);
        CLOG(INFO, "History") << "Caught up: have Bucket0Hash[" << i
                              << "] = " << hexAbbrev(haveBucket0Hash);

        CLOG(INFO, "History") << "Caught up: want Bucket1Hash[" << i
                              << "] = " << hexAbbrev(wantBucket1Hash);
        CLOG(INFO, "History") << "Caught up: have Bucket1Hash[" << i
                              << "] = " << hexAbbrev(haveBucket1Hash);

        CHECK(nextLedger == haveSeq + 1);
        CHECK(wantSeq == haveSeq);
        CHECK(wantBucketListHash == haveBucketListHash);
        CHECK(wantHash == haveHash);

        CHECK(app2->getBucketManager().getBucketByHash(wantBucket0Hash));
        CHECK(app2->getBucketManager().getBucketByHash(wantBucket1Hash));
        CHECK(wantBucket0Hash == haveBucket0Hash);
        CHECK(wantBucket1Hash == haveBucket1Hash);

        auto haveRootBalance = rootBalances.at(i);
        auto haveAliceBalance = aliceBalances.at(i);
        auto haveBobBalance = bobBalances.at(i);
        auto haveCarolBalance = carolBalances.at(i);

        auto haveRootSeq = rootSeqs.at(i);
        auto haveAliceSeq = aliceSeqs.at(i);
        auto haveBobSeq = bobSeqs.at(i);
        auto haveCarolSeq = carolSeqs.at(i);

        auto wantRootBalance = root.getBalance();
        auto wantAliceBalance = alice.getBalance();
        auto wantBobBalance = bob.getBalance();
        auto wantCarolBalance = carol.getBalance();

        auto wantRootSeq = root.loadSequenceNumber();
        auto wantAliceSeq = alice.loadSequenceNumber();
        auto wantBobSeq = bob.loadSequenceNumber();
        auto wantCarolSeq = carol.loadSequenceNumber();

        CHECK(haveRootBalance == wantRootBalance);
        CHECK(haveAliceBalance == wantAliceBalance);
        CHECK(haveBobBalance == wantBobBalance);
        CHECK(haveCarolBalance == wantCarolBalance);

        CHECK(haveRootSeq == wantRootSeq);
        CHECK(haveAliceSeq == wantAliceSeq);
        CHECK(haveBobSeq == wantBobSeq);
        CHECK(haveCarolSeq == wantCarolSeq);
    }

    app.getLedgerManager().checkDbState();
    return true;
}

CatchupMetrics
HistoryTests::getCatchupMetrics(Application::pointer app)
{
    auto& getHistoryArchiveStateSuccess = app->getMetrics().NewMeter(
        {"history", "download-history-archive-state", "success"}, "event");
    uint32_t historyArchiveStatesDownloaded =
        getHistoryArchiveStateSuccess.count();

    auto& downloadLedgersCached = app->getMetrics().NewMeter(
        {"history", "download-ledger", "cached"}, "event");
    auto& downloadLedgersSuccess = app->getMetrics().NewMeter(
        {"history", "download-ledger", "success"}, "event");

    uint32_t ledgersDownloaded =
        downloadLedgersSuccess.count() + downloadLedgersCached.count();

    auto& verifyLedgerSuccess = app->getMetrics().NewMeter(
        {"history", "verify-ledger", "success"}, "event");
    auto& verifyLedgerChainFailure = app->getMetrics().NewMeter(
        {"history", "verify-ledger-chain", "failure"}, "event");

    uint32_t ledgersVerified = verifyLedgerSuccess.count();
    uint32_t ledgerChainsVerificationFailed = verifyLedgerChainFailure.count();

    auto& downloadBucketSuccess = app->getMetrics().NewMeter(
        {"history", "download-bucket", "success"}, "event");

    uint32_t bucketsDownloaded = downloadBucketSuccess.count();

    auto& bucketApplySuccess = app->getMetrics().NewMeter(
        {"history", "bucket-apply", "success"}, "event");

    uint32_t bucketsApplied = bucketApplySuccess.count();

    auto& downloadTransactionsCached = app->getMetrics().NewMeter(
        {"history", "download-transactions", " cached "}, "event");
    auto& downloadTransactionsSuccess = app->getMetrics().NewMeter(
        {"history", "download-transactions", "success"}, "event");

    uint32_t transactionsDownloaded = downloadTransactionsSuccess.count() +
                                      downloadTransactionsCached.count();

    auto& applyLedgerSuccess = app->getMetrics().NewMeter(
        {"history", "apply-ledger", "success"}, "event");

    uint32_t transactionsApplied = applyLedgerSuccess.count();

    return CatchupMetrics{
        historyArchiveStatesDownloaded, ledgersDownloaded,  ledgersVerified,
        ledgerChainsVerificationFailed, bucketsDownloaded,  bucketsApplied,
        transactionsDownloaded,         transactionsApplied};
}

CatchupPerformedWork
HistoryTests::computeCatchupPerformedWork(
    uint32_t lastClosedLedger, CatchupConfiguration const& catchupConfiguration,
    HistoryManager const& historyManager)
{
    auto catchupRange = CatchupWork::makeCatchupRange(
        lastClosedLedger, catchupConfiguration, historyManager);
    auto checkpointRange = CheckpointRange{catchupRange.first, historyManager};

    uint32_t historyArchiveStatesDownloaded = 1;
    if (catchupRange.second &&
        checkpointRange.first() != checkpointRange.last())
    {
        historyArchiveStatesDownloaded++;
    }

    uint32_t filesDownloaded = checkpointRange.count();
    uint32_t ledgersVerified =
        checkpointRange.count() * checkpointRange.frequency();
    if (checkpointRange.first() == checkpointRange.frequency() - 1)
    {
        ledgersVerified--;
    }
    uint32_t transactionsApplied = 0;
    if (catchupRange.second)
    {
        transactionsApplied =
            catchupConfiguration.toLedger() - checkpointRange.first();
    }
    else
    {
        transactionsApplied =
            catchupConfiguration.toLedger() - lastClosedLedger;
    }
    return {historyArchiveStatesDownloaded,
            filesDownloaded,
            ledgersVerified,
            0,
            catchupRange.second,
            catchupRange.second,
            filesDownloaded,
            transactionsApplied};
}

TEST_CASE_METHOD(HistoryTests, "History publish", "[history]")
{
    generateAndPublishInitialHistory(1);
}

static std::string
resumeModeName(uint32_t count)
{
    switch (count)
    {
    case 0:
        return "CATCHUP_MINIMAL";
    case std::numeric_limits<uint32_t>::max():
        return "CATCHUP_COMPLETE";
    default:
        return "CATCHUP_RECENT";
    }
}

static std::string
dbModeName(Config::TestDbMode mode)
{
    switch (mode)
    {
    case Config::TESTDB_IN_MEMORY_SQLITE:
        return "TESTDB_IN_MEMORY_SQLITE";
    case Config::TESTDB_ON_DISK_SQLITE:
        return "TESTDB_ON_DISK_SQLITE";
#ifdef USE_POSTGRES
    case Config::TESTDB_POSTGRESQL:
        return "TESTDB_POSTGRESQL";
#endif
    default:
        abort();
    }
}

TEST_CASE_METHOD(HistoryTests, "Full history catchup",
                 "[history][historycatchup]")
{
    generateAndPublishInitialHistory(3);

    uint32_t initLedger = app.getLedgerManager().getLastClosedLedgerNum();

    std::vector<Application::pointer> apps;

    std::vector<uint32_t> counts = {0, std::numeric_limits<uint32_t>::max(),
                                    60};

    std::vector<Config::TestDbMode> dbModes = {Config::TESTDB_IN_MEMORY_SQLITE,
                                               Config::TESTDB_ON_DISK_SQLITE};
#ifdef USE_POSTGRES
    if (!force_sqlite)
        dbModes.push_back(Config::TESTDB_POSTGRESQL);
#endif

    for (auto dbMode : dbModes)
    {
        for (auto count : counts)
        {
            auto app = catchupNewApplication(initLedger, count, false, dbMode,
                                             std::string("full, ") +
                                                 resumeModeName(count) + ", " +
                                                 dbModeName(dbMode));
            apps.push_back(app);
        }
    }
}

TEST_CASE_METHOD(HistoryTests, "History publish queueing",
                 "[history][historydelay][historycatchup]")
{
    generateAndPublishInitialHistory(1);

    auto& hm = app.getHistoryManager();

    while (hm.getPublishQueueCount() < 4)
    {
        generateRandomLedger();
    }
    CLOG(INFO, "History") << "publish-delay count: "
                          << hm.getPublishDelayCount();

    while (hm.getPublishSuccessCount() < hm.getPublishQueueCount())
    {
        CHECK(hm.getPublishFailureCount() == 0);
        app.getClock().crank(true);
    }

    auto initLedger = app.getLedgerManager().getLastClosedLedgerNum();
    auto app2 =
        catchupNewApplication(initLedger, std::numeric_limits<uint32_t>::max(),
                              false, Config::TESTDB_IN_MEMORY_SQLITE,
                              std::string("Catchup to delayed history"));
    CHECK(app2->getLedgerManager().getLedgerNum() ==
          app.getLedgerManager().getLedgerNum());
}

TEST_CASE_METHOD(HistoryTests, "History prefix catchup",
                 "[history][historycatchup][prefixcatchup]")
{
    generateAndPublishInitialHistory(3);
    std::vector<Application::pointer> apps;

    // First attempt catchup to 10, prefix of 64. Should round up to 64.
    // Should replay the 64th (since it gets externalized) and land on 65.
    auto app = catchupNewApplication(
        10, std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE,
        std::string("Catchup to prefix of published history"));
    apps.push_back(app);
    uint32_t freq = apps.back()->getHistoryManager().getCheckpointFrequency();
    CHECK(apps.back()->getLedgerManager().getLedgerNum() == freq + 1);

    // Then attempt catchup to 74, prefix of 128. Should round up to 128.
    // Should replay the 64th (since it gets externalized) and land on 129.
    app = catchupNewApplication(
        freq + 10, std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE,
        std::string("Catchup to second prefix of published history"));
    apps.push_back(app);
    CHECK(apps.back()->getLedgerManager().getLedgerNum() == 2 * freq + 1);
}

TEST_CASE_METHOD(HistoryTests, "Publish/catchup alternation, with stall",
                 "[history][historycatchup][catchupalternation]")
{
    // Publish in app, catch up in app2 and app3.
    // App2 will catch up using CATCHUP_COMPLETE, app3 will use
    // CATCHUP_MINIMAL.
    generateAndPublishInitialHistory(3);

    Application::pointer app2, app3;

    auto& lm = app.getLedgerManager();

    uint32_t initLedger = lm.getLastClosedLedgerNum();

    app2 = catchupNewApplication(
        initLedger, std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE, std::string("app2"));

    app3 = catchupNewApplication(initLedger, 0, false,
                                 Config::TESTDB_IN_MEMORY_SQLITE,
                                 std::string("app3"));

    CHECK(app2->getLedgerManager().getLedgerNum() == lm.getLedgerNum());
    CHECK(app3->getLedgerManager().getLedgerNum() == lm.getLedgerNum());

    for (size_t i = 1; i < 4; ++i)
    {
        // Now alternate between publishing new stuff and catching up to it.
        generateAndPublishHistory(i);

        initLedger = lm.getLastClosedLedgerNum();

        catchupApplication(initLedger, std::numeric_limits<uint32_t>::max(),
                           false, app2);
        catchupApplication(initLedger, 0, false, app3);

        CHECK(app2->getLedgerManager().getLedgerNum() == lm.getLedgerNum());
        CHECK(app3->getLedgerManager().getLedgerNum() == lm.getLedgerNum());
    }

    // By now we should have had 3 + 1 + 2 + 3 = 9 publishes, and should
    // have advanced 1 ledger in to the 9th block.
    uint32_t freq = app2->getHistoryManager().getCheckpointFrequency();
    CHECK(app2->getLedgerManager().getLedgerNum() == 9 * freq + 1);
    CHECK(app3->getLedgerManager().getLedgerNum() == 9 * freq + 1);

    // Finally, publish a little more history than the last publish-point
    // but not enough to get to the _next_ publish-point:

    generateRandomLedger();
    generateRandomLedger();
    generateRandomLedger();

    // Attempting to catch up here should _stall_. We evaluate stalling
    // by providing 30 cranks of the event loop and assuming that failure
    // to catch up within that time means 'stalled'.

    bool caughtup = false;
    initLedger = lm.getLastClosedLedgerNum();

    caughtup = catchupApplication(
        initLedger, std::numeric_limits<uint32_t>::max(), false, app2);
    CHECK(!caughtup);
    caughtup = catchupApplication(initLedger, 0, false, app3);
    CHECK(!caughtup);

    // Now complete this publish cycle and confirm that the stalled apps
    // will catch up.
    generateAndPublishHistory(1);
    caughtup = catchupApplication(
        initLedger, std::numeric_limits<uint32_t>::max(), false, app2, false);
    CHECK(caughtup);
    caughtup = catchupApplication(initLedger, 0, false, app3, false);
    CHECK(caughtup);
}

TEST_CASE_METHOD(HistoryTests, "Repair missing buckets via history",
                 "[history][historybucketrepair]")
{
    generateAndPublishInitialHistory(1);

    // Forcibly resolve any merges in progress, so we have a calm state to
    // repair;
    // NB: we cannot repair lost buckets from merges-in-progress, as they're
    // not
    // necessarily _published_ anywhere.
    HistoryArchiveState has(app.getLedgerManager().getLastClosedLedgerNum(),
                            app.getBucketManager().getBucketList());
    has.resolveAllFutures();
    auto state = has.toString();

    auto cfg2 = getTestConfig(1);
    cfg2.BUCKET_DIR_PATH += "2";
    auto app2 =
        Application::create(clock, mConfigurator->configure(cfg2, false));
    app2->getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                        state);

    app2->start();

    auto hash1 = appPtr->getBucketManager().getBucketList().getHash();
    auto hash2 = app2->getBucketManager().getBucketList().getHash();
    CHECK(hash1 == hash2);
}

TEST_CASE_METHOD(HistoryTests, "Repair missing buckets fails",
                 "[history][historybucketrepair]")
{
    generateAndPublishInitialHistory(1);

    // Forcibly resolve any merges in progress, so we have a calm state to
    // repair;
    // NB: we cannot repair lost buckets from merges-in-progress, as they're
    // not
    // necessarily _published_ anywhere.
    HistoryArchiveState has(app.getLedgerManager().getLastClosedLedgerNum(),
                            app.getBucketManager().getBucketList());
    has.resolveAllFutures();
    auto state = has.toString();

    // Delete buckets from the archive before proceding.
    // This means startup will fail.
    auto dir = mConfigurator->getArchiveDirName();
    REQUIRE(!dir.empty());
    fs::deltree(dir + "/bucket");

    auto cfg2 = getTestConfig(1);
    cfg2.BUCKET_DIR_PATH += "2";
    auto app2 =
        Application::create(clock, mConfigurator->configure(cfg2, false));
    app2->getPersistentState().setState(PersistentState::kHistoryArchiveState,
                                        state);

    REQUIRE_THROWS(app2->start());

    while (app2->getProcessManager().getNumRunningProcesses() != 0)
    {
        try
        {
            app2->getClock().crank(false);
        }
        catch (...)
        {
            // see https://github.com/stellar/stellar-core/issues/1250
            // we expect to get "Unable to restore last-known ledger state"
            // several more times
        }
    }
}

class S3Configurator : public Configurator
{
  public:
    Config&
    configure(Config& cfg, bool writable) const override
    {
        char const* s3bucket = getenv("S3BUCKET");
        if (!s3bucket)
        {
            throw std::runtime_error("s3 test requires S3BUCKET env var");
        }
        std::string s3b(s3bucket);
        if (s3b.find("s3://") != 0)
        {
            s3b = std::string("s3://") + s3b;
        }
        std::string getCmd = "aws s3 cp " + s3b + "/{0} {1}";
        std::string putCmd = "";
        std::string mkdirCmd = "";
        if (writable)
        {
            putCmd = "aws s3 cp {0} " + s3b + "/{1}";
        }
        cfg.HISTORY["test"] =
            std::make_shared<HistoryArchive>("test", getCmd, putCmd, mkdirCmd);
        return cfg;
    }
};

class S3HistoryTests : public HistoryTests
{
  public:
    S3HistoryTests() : HistoryTests(std::make_shared<S3Configurator>())
    {
    }
};

TEST_CASE_METHOD(S3HistoryTests, "Publish/catchup via s3", "[hide][s3]")
{
    generateAndPublishInitialHistory(3);
    auto app2 = catchupNewApplication(
        app.getLedgerManager().getCurrentLedgerHeader().ledgerSeq,
        std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE, "s3");
}

TEST_CASE("persist publish queue", "[history]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    cfg.MAX_CONCURRENT_SUBPROCESSES = 0;
    cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
    TmpDirConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);

    {
        VirtualClock clock;
        Application::pointer app0 = Application::create(clock, cfg);
        app0->start();
        auto& hm0 = app0->getHistoryManager();
        while (hm0.getPublishQueueCount() < 5)
        {
            clock.crank(true);
        }
        // We should have published nothing and have the first
        // checkpoint still queued.
        CHECK(hm0.getPublishSuccessCount() == 0);
        CHECK(hm0.getMinLedgerQueuedToPublish() == 7);
        while (clock.cancelAllEvents() ||
               app0->getProcessManager().getNumRunningProcesses() > 0)
        {
            clock.crank(true);
        }
        LOG(INFO) << app0->isStopping();

        // Trim history after publishing.
        ExternalQueue ps(*app0);
        ps.process();
    }

    cfg.MAX_CONCURRENT_SUBPROCESSES = 32;

    {
        VirtualClock clock;
        Application::pointer app1 = Application::create(clock, cfg, false);
        HistoryManager::initializeHistoryArchive(*app1, "test");
        for (size_t i = 0; i < 100; ++i)
            clock.crank(false);
        app1->start();
        auto& hm1 = app1->getHistoryManager();
        while (hm1.getPublishSuccessCount() < 5)
        {
            clock.crank(true);

            // Trim history after publishing whenever possible.
            ExternalQueue ps(*app1);
            ps.process();
        }
        // We should have either an empty publish queue or a
        // ledger sometime after the 5th checkpoint
        auto minLedger = hm1.getMinLedgerQueuedToPublish();
        LOG(INFO) << "minLedger " << minLedger;
        bool okQueue = minLedger == 0 || minLedger >= 35;
        CHECK(okQueue);
        clock.cancelAllEvents();
        while (clock.cancelAllEvents() ||
               app1->getProcessManager().getNumRunningProcesses() > 0)
        {
            clock.crank(true);
        }
        LOG(INFO) << app1->isStopping();
    }
}

// The idea with this test is that we join a network and somehow get a gap
// in the SCP voting sequence while we're trying to catchup.  This should
// cause catchup to fail, but that failure should itself just flush the
// ledgermanager's buffer and get kicked back into catchup mode when the
// network moves further ahead.

// (Both the hard-failure and the clear/reset weren't working when this
// test was written)
TEST_CASE_METHOD(HistoryTests, "too far behind / catchup restart",
                 "[history][catchupstall]")
{
    generateAndPublishInitialHistory(1);

    // Catch up successfully the first time
    auto app2 = catchupNewApplication(
        app.getLedgerManager().getCurrentLedgerHeader().ledgerSeq,
        std::numeric_limits<uint32_t>::max(), false,
        Config::TESTDB_IN_MEMORY_SQLITE, "app2");

    // Now generate a little more history
    generateAndPublishHistory(1);

    bool caughtup = false;
    auto init = app2->getLedgerManager().getLastClosedLedgerNum() + 2;

    // Now start a catchup on that _fails_ due to a gap
    LOG(INFO) << "Starting BROKEN catchup (with gap) from " << init;
    caughtup = catchupApplication(init, std::numeric_limits<uint32_t>::max(),
                                  false, app2, true, init + 10);
    assert(!caughtup);

    app2->getWorkManager().clearChildren();

    // Now generate a little more history
    generateAndPublishHistory(1);

    // And catchup successfully
    init = app.getLedgerManager().getLastClosedLedgerNum();
    caughtup = catchupApplication(init, std::numeric_limits<uint32_t>::max(),
                                  false, app2);
    assert(caughtup);
}

/*
 * Test a variety of orderings of CATCHUP_RECENT mode, to shake out boundary
 * cases.
 */
TEST_CASE_METHOD(HistoryTests, "Catchup recent", "[history][catchuprecent]")
{
    auto dbMode = Config::TESTDB_IN_MEMORY_SQLITE;
    std::vector<Application::pointer> apps;

    generateAndPublishInitialHistory(3);

    // Network has published 0x3f (63), 0x7f (127) and 0xbf (191)
    // Network is currently sitting on ledger 0xc0 (192)
    uint32_t initLedger = app.getLedgerManager().getLastClosedLedgerNum();

    // Check that isolated catchups work at a variety of boundary
    // conditions relative to the size of a checkpoint:
    std::vector<uint32_t> recents = {0,   1,   2,   31,  32,  33,  62,  63,
                                     64,  65,  66,  126, 127, 128, 129, 130,
                                     190, 191, 192, 193, 194, 1000};

    for (auto r : recents)
    {
        auto name = std::string("catchup-recent-") + std::to_string(r);
        apps.push_back(
            catchupNewApplication(initLedger, r, false, dbMode, name));
    }

    // Now push network along a little bit and see that they can all still
    // catch up properly.
    generateAndPublishHistory(2);
    initLedger = app.getLedgerManager().getLastClosedLedgerNum();

    for (auto a : apps)
    {
        catchupApplication(initLedger, 80, false, a);
    }

    // Now push network along a _lot_ futher along see that they can all
    // still
    // catch up properly.
    generateAndPublishHistory(25);
    initLedger = app.getLedgerManager().getLastClosedLedgerNum();

    for (auto a : apps)
    {
        catchupApplication(initLedger, 80, false, a);
    }
}

/*
 * Test a variety of LCL/initLedger/count modes.
 */
TEST_CASE_METHOD(HistoryTests, "Catchup manual", "[history][catchupmanual]")
{
    auto dbMode = Config::TESTDB_IN_MEMORY_SQLITE;
    std::vector<Application::pointer> apps;

    generateAndPublishInitialHistory(6);
    auto initLedger = app.getLedgerManager().getLastClosedLedgerNum();
    REQUIRE(initLedger == 383);

    for (auto const& test : stellar::gCatchupRangeCases)
    {
        auto lastClosedLedger = test.first;
        auto configuration = test.second;
        auto name = fmt::format("lcl = {}, to ledger = {}, count = {}",
                                lastClosedLedger, configuration.toLedger(),
                                configuration.count());
        // manual catchup-recent
        auto app =
            catchupNewApplication(configuration.toLedger(),
                                  configuration.count(), true, dbMode, name);
        // manual catchup-complete
        catchupApplication(initLedger, std::numeric_limits<uint32_t>::max(),
                           true, app);
        apps.push_back(app);
    }

    // Now push network along a little bit and see that they can all still
    // catch up properly.
    generateAndPublishHistory(2);
    initLedger = app.getLedgerManager().getLastClosedLedgerNum();

    for (auto a : apps)
    {
        catchupApplication(initLedger, 80, false, a);
    }
}

// Check that initializing a history store that already exists, fails.
TEST_CASE("initialize existing history store fails", "[history]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    TmpDirConfigurator tcfg;
    cfg = tcfg.configure(cfg, true);

    {
        VirtualClock clock;
        Application::pointer app = Application::create(clock, cfg);
        REQUIRE(HistoryManager::initializeHistoryArchive(*app, "test"));
    }

    {
        VirtualClock clock;
        Application::pointer app = Application::create(clock, cfg);
        REQUIRE(!HistoryManager::initializeHistoryArchive(*app, "test"));
    }
}
