/*
* Copyright 2008, 2009, 2010 Free Software Foundation, Inc.
* Copyright 2012 Thomas Cooper <tacooper@vt.edu>
*
* This software is distributed under the terms of the GNU Affero Public License.
* See the COPYING file in the main directory for details.
*
* This use of this software may be subject to additional restrictions.
* See the LEGAL file in the main directory for details.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/


#define NDEBUG


#include "GSML1FEC.h"
#include "GSMCommon.h"
#include "GSMSAPMux.h"
#include "GSMConfig.h"
#include "GSMTDMA.h"
#include "GSMTAPDump.h"
#include <TRXManager.h>
#include <assert.h>
#include <math.h>

using namespace std;
using namespace GSM;


/*

	Notes on reading the GSM specifications.

	Every FEC section in GSM 05.03 uses standard names for the bits at
	different stages of the encoding/decoding process.

	This is all described formally in GSM 05.03 2.2.

	"d"	-- data bits.  The actual payloads from L2 and the vocoders.
	"p" -- parity bits.  These are calculated from d.
	"u" -- uncoded bits.  A concatenation of d, p and inner tail bits.
	"c" -- coded bits.  These are the convolutionally encoded from u.
	"i" -- interleaved bits.  These are the output of the interleaver.
	"e" -- "encrypted" bits.  These are the channel bits in the radio bursts.

	The "e" bits are call "encrypted" even when encryption is not used.

	The encoding process is:

	L2 -> d -> -> calc p -> u -> c -> i -> e -> radio bursts

	The decoding process is:

	radio bursts -> e -> i -> c -> u -> check p -> d -> L2

	Bit ordering in d is LSB-first in each octet.
	Bit ordering everywhere else in the OpenBTS code is MSB-first
	in every field to give contiguous fields across byte boundaries.
	We use the BitVector::LSB8MSB() method to translate.

*/





/**@name Power control utility functions based on GSM 05.05 4.1.1 */
//@{

/** Power control codes for GSM400, GSM850, EGSM900 from GSM 05.05 4.1.1. */
static const int powerCommandLowBand[32] = 
{
	39, 39, 39, 37,	// 0-3
	35, 33, 31, 29,	// 4-7
	27, 25, 23, 21,	// 8-11
	19, 17, 15, 13,	// 12-15
	11, 9, 7, 5,	// 16-19
	5, 5, 5, 5,		// 20-23
	5, 5, 5, 5,		// 24-27
	5, 5, 5, 5		// 28-31
};

/** Power control codes for DCS1800 from GSM 05.05 4.1.1. */
static const int powerCommand1800[32] =
{
	30, 28, 26, 24,	// 0-3
	22, 20, 18, 16,	// 4-7
	14, 12, 10, 8,	// 8-11
	6, 4, 2, 0,		// 12-15
	0, 0, 0, 0,		// 16-19
	0, 0, 0, 0,		// 20-23
	0, 0, 0, 0,		// 24-27
	0, 36, 24, 23	// 28-31
};

/** Power control codes for PCS1900 from GSM 05.05 4.1.1. */
static const int powerCommand1900[32] =
{
	30, 28, 26, 24,	// 0-3
	22, 20, 18, 16,	// 4-7
	14, 12, 10, 8,	// 8-11
	6, 4, 2, 0,		// 12-15
	0, 0, 0, 0,		// 16-19
	0, 0, 0, 0,		// 20-23
	0, 0, 0, 0,		// 24-27
	0, 0, 0, 0,		// 28-31
};


const int* pickTable()
{
	switch (gBTSL1.band()) {
		case GSM850:
		case EGSM900:
			return powerCommandLowBand;
			break;
		case DCS1800:
			return powerCommand1800;
			break;
		case PCS1900:
			return powerCommand1900;
			break;
		default: return NULL;
	}
}


int decodePower(unsigned code)
{
	static const int *table = pickTable();
	assert(table);
	return table[code];

}


/** Given a power level in dBm, encode the control code. */
unsigned encodePower(int power)
{
	static const int *table = pickTable();
	assert(table);
	unsigned minErr = abs(power - table[0]);
	unsigned code = 0;
	for (int i=1; i<32; i++) {
		unsigned thisErr = abs(power - table[i]);
		if (thisErr==0) return i;
		if (thisErr<minErr) {
			minErr = thisErr;
			code = i;
		}
	}
	return code;
}


//@}





/*
	L1Encoder base class methods.
*/


L1Encoder::L1Encoder(unsigned wTN, const TDMAMapping& wMapping, L1FEC *wParent)
	:mDownstream(NULL),
	mUpstream(NULL),
	mMapping(wMapping),mTN(wTN),
	mTSC(gBTSL1.BCC()),				// Note that TSC is hardcoded to the BCC.
	mParent(wParent),
	mTotalBursts(0),
	mPrevWriteTime(gBTSL1.time().FN(),wTN),
	mNextWriteTime(gBTSL1.time().FN(),wTN),
	mRunning(false),mActive(false)
{
	assert(mMapping.allowedSlot(mTN));
	assert(mMapping.downlink());
	mNextWriteTime.rollForward(mMapping.frameMapping(0),mMapping.repeatLength());
	mPrevWriteTime.rollForward(mMapping.frameMapping(0),mMapping.repeatLength());
	// Compatibility with C0 will be checked in the ARFCNManager.
}


void L1Encoder::rollForward()
{
	// Calculate the TDMA paramters for the next transmission.
	// This implements GSM 05.02 Clause 7 for the transmit side.
	mPrevWriteTime = mNextWriteTime;
	mTotalBursts++;
	mNextWriteTime.rollForward(mMapping.frameMapping(mTotalBursts),mMapping.repeatLength());
	signalNextWtime();
}

void L1Encoder::signalNextWtime()
{
	if (mUpstream)
		mUpstream->signalNextWtime(mNextWriteTime);
}

unsigned L1Encoder::ARFCN() const
{
	assert(mDownstream);
	return mDownstream->ARFCN();
}


TypeAndOffset L1Encoder::typeAndOffset() const
{
	return mMapping.typeAndOffset();
}


void L1Encoder::open()
{
	OBJLOG(DEBUG) << "L1Encoder";
	mLock.lock();
	if (!mRunning) start();
	mTotalBursts=0;
	mActive = true;
	resync();
	mLock.unlock();
}


void L1Encoder::close()
{
	// Don't return until the channel is fully closed.
	OBJLOG(DEBUG) << "L1Encodere";
	mLock.lock();
	mActive = false;
	sendIdleFill();
	mLock.unlock();
}


bool L1Encoder::active() const
{
	mLock.lock();
	bool retVal = mActive;
	const L1Decoder *sib = sibling();
	if (sib) retVal = mActive && (!sib->recyclable());
	mLock.unlock();
	return retVal;
}


L1Decoder* L1Encoder::sibling()
{
	if (!mParent) return NULL;
	return mParent->decoder();
}


const L1Decoder* L1Encoder::sibling() const
{
	if (!mParent) return NULL;
	return mParent->decoder();
}


void L1Encoder::resync()
{
	// If the encoder's clock is far from the current BTS clock,
	// get it caught up to something reasonable.
	Time now = gBTSL1.time();
	int32_t delta = mNextWriteTime-now;
	OBJLOG(DEEPDEBUG) << "L1Encoder next=" << mNextWriteTime << " now=" << now << " delta=" << delta;
	if ((delta<0) || (delta>(51*26))) {
		mNextWriteTime = now;
		mNextWriteTime.TN(mTN);
		mNextWriteTime.rollForward(mMapping.frameMapping(mTotalBursts),mMapping.repeatLength());
		OBJLOG(DEEPDEBUG) <<"L1Encoder RESYNC next=" << mNextWriteTime << " now=" << now;
	}
}


void L1Encoder::waitToSend() const
{
	// Block until the BTS clock catches up to the
	// mostly recently transmitted burst.
	gBTSL1.clock().wait(mPrevWriteTime);
}


void L1Encoder::sendIdleFill()
{
	// Send the L1 idle filling pattern, if any.
	// For C0, that's the dummy burst.
	// TODO -- In a multi-ARFCN system,
	// we will need to check to see if this is really C0 before sending a dummy.
	resync();
	for (unsigned i=0; i<mMapping.numFrames(); i++) {
		mFillerBurst.time(mNextWriteTime);
		mDownstream->writeHighSide(mFillerBurst);
		rollForward();
	}
}




unsigned L1Decoder::ARFCN() const
{
	assert(mParent);
	return mParent->ARFCN();
}


TypeAndOffset L1Decoder::typeAndOffset() const
{
	return mMapping.typeAndOffset();
}


void L1Decoder::open()
{
	mLock.lock();
	if (!mRunning) start();
	mFER=0.0F;
	mT3111.reset();
	mT3109.reset();
	mT3101.set();
	mActive = true;
	mLock.unlock();
}


void L1Decoder::close()
{
	mLock.lock();
	mT3101.reset();
	mT3109.reset();
	mT3111.set();
	mActive = false;
	mLock.unlock();
}

bool L1Decoder::active() const
{
	mLock.lock();
	bool retVal = mActive && !recyclable();
	mLock.unlock();
	return retVal;
}


bool L1Decoder::recyclable() const
{
	mLock.lock();
	bool retVal = mT3101.expired() || mT3109.expired() || mT3111.expired();
	mLock.unlock();
	return retVal;
}


L1Encoder* L1Decoder::sibling()
{
	if (!mParent) return NULL;
	return mParent->encoder();
}


const L1Encoder* L1Decoder::sibling() const
{
	if (!mParent) return NULL;
	return mParent->encoder();
}




void L1Decoder::countGoodFrame()
{
	static const float a = 1.0F / ((float)mFERMemory);
	static const float b = 1.0F - a;
	mFER *= b;
	OBJLOG(DEEPDEBUG) <<"L1Decoder FER=" << mFER;
}


void L1Decoder::countBadFrame()
{
	static const float a = 1.0F / ((float)mFERMemory);
	static const float b = 1.0F - a;
	mFER = b*mFER + a;
	OBJLOG(DEEPDEBUG) <<"L1Decoder FER=" << mFER;
}




void L1FEC::downstream(ARFCNManager* radio)
{
	if (mEncoder) mEncoder->downstream(radio);
	if (mDecoder) radio->installDecoder(mDecoder);
}


void L1FEC::open()
{
	if (mEncoder) mEncoder->open();
	if (mDecoder) mDecoder->open();
}

void L1FEC::close()
{
	if (mEncoder) mEncoder->close();
	if (mDecoder) mDecoder->close();
}

bool L1FEC::active() const
{
	// Encode-only channels are always active.
	// Otherwise, the decoder is the better indicator.
	if (mDecoder) return mDecoder->active();
	else return (mEncoder!=NULL);
}




void RACHL1Decoder::serviceLoop()
{
	// The service loop pulls RACH bursts from a FIFO
	// and sends them to the decoder.
	// This loop is in its own thread because
	// the allocator can potentially block and we don't 
	// want the whole receive thread to block.

	while(mRunning)
	{
		if(mActive)
		{
			RxBurst *rx = mQ.read();
			// Yes, if we wait long enough that read will timeout.
			if (rx==NULL) continue;
			writeLowSide(*rx);
			delete rx;
		}
	}
}


void *GSM::RACHL1DecoderServiceLoopAdapter(RACHL1Decoder* obj)
{
	obj->serviceLoop();
	return NULL;
}


void RACHL1Decoder::start()
{
	// Start the processing thread.
	L1Decoder::start();
	mServiceThread.start((void*(*)(void*))RACHL1DecoderServiceLoopAdapter,this);
}



void RACHL1Decoder::writeLowSide(const RxBurst& burst)
{
	// The L1 FEC for the RACH is defined in GSM 05.03 4.6.

	// Decode the burst.
	const SoftVector e(burst.segment(49,36));
	e.decode(mVCoder,mU);

	// To check validity, we have 4 tail bits and 6 parity bits.
	// False alarm rate for random inputs is 1/1024.

	// Check the tail bits -- should all the zero.
	if (mU.peekField(14,4)) {
		countBadFrame();
		return;
	}

	// Check the parity.
	// The parity word is XOR'd with the BSIC. (GSM 05.03 4.6.)
	unsigned sentParity = ~mU.peekField(8,6);
	unsigned checkParity = mD.parity(mParity);
	unsigned encodedBSIC = (sentParity ^ checkParity) & 0x03f;
	if (encodedBSIC != gBTSL1.BSIC()) {
		countBadFrame();
		return;
	}

	// We got a valid RACH burst.
	// The "payload" is an 8-bit field, "RA", defined in GSM 04.08 9.1.8.
	// The channel assignment procedure is in GSM 04.08 3.3.1.1.3.
	// It requires knowledge of the RA value and the burst receive time.
	// The RACH L2 is so thin that we don't even need code for it.
	// Just pass the required information directly to the control layer.

	countGoodFrame();
	mD.LSB8MSB();
	unsigned RA = mD.peekField(0,8);

	int initialTA = (int)(burst.timingError() + 0.5F);
	if (initialTA<0) initialTA=0;
	if (initialTA>63) initialTA=63;

	OBJLOG(INFO) <<"RACHL1Decoder rx: RA=" << RA << " time=" << burst.time() <<
		" RSSI=" << burst.RSSI() << " timingError=" << burst.timingError() <<
		" TA=" << initialTA;

	// Send to GSMTAP
	gWriteGSMTAP(0 /* no ARFCN()! */, burst.time().TN(), burst.time().FN(), 
		typeAndOffset(), false, true, mD, GSMTAP_BURST_ACCESS);

	/* Build L2Frame and send burst up to OsmoSAPMux */
	assert(mUpstream);

	L2Frame frame(mD, DATA);

	mUpstream->writeLowSide(frame, burst.time(), burst.RSSI(), initialTA, mFER);
}







/*
	XCCHL1Encoder and Decoder methods.
	The "XCCH" L1 components are based on GSM 05.03 4.1.
	These are the most commonly used control channel L1 format
	in GSM and are offer here as examples.
*/




XCCHL1Decoder::XCCHL1Decoder(
		unsigned wTN,
		const TDMAMapping& wMapping,
		L1FEC *wParent)
	:L1Decoder(wTN,wMapping,wParent),
	mBlockCoder(0x10004820009ULL, 40, 224),
	mC(456), mU(228),
	mP(mU.segment(184,40)),mDP(mU.head(224)),mD(mU.head(184)),
	mRSSICounter(0)
{
	for (int i=0; i<4; i++) {
		mI[i] = SoftVector(114);
		// Fill with zeros just to make Valgrind happy.
		mI[i].fill(.0);
	}

	for (int i=0; i<4; i++) mRSSI[i]=0.0F;
}



void XCCHL1Decoder::writeLowSide(const RxBurst& inBurst)
{
	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder " << inBurst;
	// If the channel is closed, ignore the burst.
	if (!active()) {
		OBJLOG(DEBUG) <<"XCCHL1Decoder not active, ignoring input";
		return;
	}
	// Accept the burst into the deinterleaving buffer.
	// Return true if we are ready to interleave.
	if (!processBurst(inBurst)) return;
	deinterleave();
	if (decode()) {
		countGoodFrame();
		mD.LSB8MSB();
		handleGoodFrame();
	} else {
		countBadFrame();
	}
}


bool XCCHL1Decoder::processBurst(const RxBurst& inBurst)
{
	/* SACCH-like processing of RSSI and TimingError */
	// TODO -- One quick test of burst validity is to look at the tail bits.
	// We could do that as a double-check against putting garbage into
	// the interleaver or accepting bad parameters.

	// Get the physical parameters of the burst.
	// The actual phone settings change every 4 bursts,
	// so average over all 4.
	// RSSI is dB wrt full scale.
	mRSSI[mRSSICounter] = inBurst.RSSI();
	// Timing error is a float in symbol intervals.
	mTimingError[mRSSICounter] = inBurst.timingError();

	OBJLOG(DEBUG) << "XCCHL1Decoder #" << mRSSICounter << ": RSSI=" << 
		inBurst.RSSI() << " timingError=" << inBurst.timingError();

	// This flag is used as a half-ass semaphore.
	// It is cleared when the new value is read.
	mPhyNew = true;
	mRSSICounter++;
	if (mRSSICounter>3) mRSSICounter=0;


	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder " << inBurst;
	// Accept the burst into the deinterleaving buffer.
	// Return true if we are ready to interleave.

	// TODO -- One quick test of burst validity is to look at the tail bits.
	// We could do that as a double-check against putting garbage into
	// the interleaver or accepting bad parameters.

	// The reverse index runs 0..3 as the bursts arrive.
	// It is the "B" index of GSM 05.03 4.1.4 and 4.1.5.
	int B = mMapping.reverseMapping(inBurst.time().FN()) % 4;
	// A negative value means that the demux is misconfigured.
	assert(B>=0);

	// Pull the data fields (e-bits) out of the burst and put them into i[B][].
	// GSM 05.03 4.1.5
	inBurst.data1().copyToSegment(mI[B],0);
	inBurst.data2().copyToSegment(mI[B],57);

	// If the burst index is 0, save the time
	if (B==0)
		mReadTime = inBurst.time();

	// If the burst index is 3, then this is the last burst in the L2 frame.
	// Return true to indicate that we are ready to deinterleave.
	return B==3;

	// TODO -- This is sub-optimal because it ignores the case
	// where the B==3 burst is simply missing, even though the soft decoder
	// might actually be able to recover the frame.
	// It also allows for the mixing of bursts from different frames.
	// If we were more clever, we'd be watching for B to roll over as well.
}




void XCCHL1Decoder::deinterleave()
{
	// Deinterleave i[][] to c[].
	// This comes directly from GSM 05.03, 4.1.4.
	for (int k=0; k<456; k++) {
		int B = k%4;
		int j = 2*((49*k) % 57) + ((k%8)/4);
		mC[k] = mI[B][j];
		// Mark this i[][] bit as unknown now.
		// This makes it possible for the soft decoder to work around
		// a missing burst.
		mI[B][j] = 0.5F;
	}
}


bool XCCHL1Decoder::decode()
{
	// Apply the convolutional decoder and parity check.
	// Return true if we recovered a good L2 frame.

	// Convolutional decoding c[] to u[].
	// GSM 05.03 4.1.3
	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder << mC";
	mC.decode(mVCoder,mU);
	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder << mU";

	// The GSM L1 u-frame has a 40-bit parity field.
	// False detections are EXTREMELY rare.
	// Parity check of u[].
	// GSM 05.03 4.1.2.
	mP.invert();							// parity is inverted
	// The syndrome should be zero.
	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder d[]:p[]=" << mDP;
	unsigned syndrome = mBlockCoder.syndrome(mDP);
	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder syndrome=" << hex << syndrome << dec;
	return (syndrome==0);
}



void XCCHL1Decoder::handleGoodFrame()
{
	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder u[]=" << mU;
	mLock.lock();
	// Keep T3109 from timing out.
	mT3109.set();
	// If this is the first good frame of a new transaction,
	// stop T3101 and tell L2 we're alive down here.
	if (mT3101.active()) {
		mT3101.reset();
		//if (mUpstream!=NULL) mUpstream->writeLowSide(L2Frame(ESTABLISH));
	}
	mLock.unlock();

	// Get the d[] bits, the actual payload in the radio channel.
	// Undo GSM's LSB-first octet encoding.
	OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder d[]=" << mD;

	if (mUpstream) {
		// Send all bits to GSMTAP
		gWriteGSMTAP(ARFCN(),TN(),mReadTime.FN(),
		             typeAndOffset(),mMapping.repeatLength()>51,true,
					 mD, 0);
		/* Build L2Frame and send burst up to OsmoSAPMux */
		const BitVector L2Part(mD.tail(headerOffset()));
		OBJLOG(DEEPDEBUG) <<"XCCHL1Decoder L2=" << L2Part;

		if(channelType() == SACCHType)
		{
			mUpstream->writeLowSideSACCH(L2Frame(L2Part,DATA), mReadTime, 
				RSSI(), TA(), mFER, actualMSPower(), actualMSTiming());
		}
		else
		{
			mUpstream->writeLowSide(L2Frame(L2Part,DATA), mReadTime, RSSI(), 
				TA(), mFER);
		}

	} else {
		OBJLOG(ERROR) << "XCCHL1Decoder with no uplink connected.";
	}
}


float XCCHL1Decoder::RSSI() const
{
	float sum=mRSSI[0]+mRSSI[1]+mRSSI[2]+mRSSI[3];
	mPhyNew=false;
	return 0.25F*sum;
}

float XCCHL1Decoder::timingError() const
{
	float sum=mTimingError[0]+mTimingError[1]+mTimingError[2]+mTimingError[3];
	mPhyNew=false;
	return 0.25F*sum;
}

int XCCHL1Decoder::TA() const
{
	int TA = (int)(timingError() + 0.5F);
	if (TA<0) TA=0;
	if (TA>63) TA=63;
	return TA;
}

void SACCHL1Decoder::handleGoodFrame()
{
	// GSM 04.04 7
	OBJLOG(DEBUG) << "SACCHL1Decoder phy header " << mU.head(16);
	mActualMSPower = decodePower(mU.peekField(3,5));
	int TAField = mU.peekField(9,7);
	if (TAField<64) mActualMSTiming = TAField;
	OBJLOG(INFO) << "SACCHL1Decoder actuals pow=" << mActualMSPower << " TA=" << mActualMSTiming;
	XCCHL1Decoder::handleGoodFrame();
}






XCCHL1Encoder::XCCHL1Encoder(
		unsigned wTN,
		const TDMAMapping& wMapping,
		L1FEC* wParent)
	:L1Encoder(wTN,wMapping,wParent),
	mBlockCoder(0x10004820009ULL, 40, 224),
	mC(456), mU(228),
	mD(mU.head(184)),mP(mU.segment(184,40))
{
	// Set up the interleaving buffers.
	for(int k = 0; k<4; k++) {
		mI[k] = BitVector(114);
		// Fill with zeros just to make Valgrind happy.
		mI[k].fill(0);
	}

	mFillerBurst = TxBurst(gDummyBurst);

	// Set up the training sequence and stealing bits
	// since they'll be the same for all bursts.

	// stealing bits for a control channel, GSM 05.03 4.2.5, 05.02 5.2.3.
	mBurst.Hl(1);
	mBurst.Hu(1);
	// training sequence, GSM 05.02 5.2.3
	gTrainingSequence[mTSC].copyToSegment(mBurst,61);

	// zero out u[] to take care of tail fields
	mU.zero();
}

void XCCHL1Encoder::writeHighSide(const L2Frame& frame)
{
	LOG(DEBUG) << "XCCHL1Encoder::writeHighSide " << frame;

	switch (frame.primitive()) {
		case DATA:
			// Encode and send data.
			if (!active()) { LOG(INFO) << "XCCHL1Encoder::writeHighSide sending on non-active channel"; }
			resync();
			sendFrame(frame);
			break;
		case ESTABLISH:
			// Open both sides of the link.
			// The phone is waiting to see the idle pattern.
			open();
			if (sibling()) sibling()->open();
			return;
		case RELEASE:
			// Normally, we get here after a DISC-DM handshake in L2.
			// Close both sides of the link, knowing that the phone will do the same.
			close();
			if (sibling()) sibling()->close();
			break;
		case ERROR:
			// If we got here, it means the link failed in L2 after several ack timeouts.
			// Close the tx side and just let the receiver L1 time out on its own.
			// Otherwise, we risk recycling the channel while the phone's still active.
			close();
			break;
		default:
			LOG(ERROR) << "unhandled primitive " << frame.primitive() << " in L2->L1";
			assert(0);
	}
}



void XCCHL1Encoder::sendFrame(const L2Frame& frame)
{
	OBJLOG(DEEPDEBUG) << "XCCHL1Encoder " << frame;
	// Make sure there's something down there to take the busts.
	if (mDownstream==NULL) {
		LOG(WARN) << "XCCHL1Encoder with no downstream";
		return;
	}

	// This comes from GSM 05.03 4.1

	// Copy the L2 frame into u[] for processing.
	// GSM 05.03 4.1.1.
	//assert(mD.size()==headerOffset()+frame.size());
	frame.copyToSegment(mU,headerOffset());

	// Send to GSMTAP (must send mU = real bits !)
	gWriteGSMTAP(ARFCN(),TN(),mNextWriteTime.FN(),
	             typeAndOffset(),mMapping.repeatLength()>51,false,mU, 0);

	// Encode data into bursts
	OBJLOG(DEEPDEBUG) << "XCCHL1Encoder d[]=" << mD;
	mD.LSB8MSB();
	OBJLOG(DEEPDEBUG) << "XCCHL1Encoder d[]=" << mD;
	encode();			// Encode u[] to c[], GSM 05.03 4.1.2 and 4.1.3.
	interleave();		// Interleave c[] to i[][], GSM 05.03 4.1.4.
	transmit();			// Send the bursts to the radio, GSM 05.03 4.1.5.
}



void XCCHL1Encoder::encode()
{
	// Perform the FEC encoding of GSM 05.03 4.1.2 and 4.1.3

	// GSM 05.03 4.1.2
	// Generate the parity bits.
	mBlockCoder.writeParityWord(mD,mP);
	OBJLOG(DEEPDEBUG) << "XCCHL1Encoder u[]=" << mU;
	// GSM 05.03 4.1.3
	// Apply the convolutional encoder.
	mU.encode(mVCoder,mC);
	OBJLOG(DEEPDEBUG) << "XCCHL1Encoder c[]=" << mC;
}



void XCCHL1Encoder::interleave()
{
	// GSM 05.03, 4.1.4.  Verbatim.
	for (int k=0; k<456; k++) {
		int B = k%4;
		int j = 2*((49*k) % 57) + ((k%8)/4);
		mI[B][j] = mC[k];
	}
}



void XCCHL1Encoder::transmit()
{
	// Format the bits into the bursts.
	// GSM 05.03 4.1.5, 05.02 5.2.3

	waitToSend();		// Don't get too far ahead of the clock.

	if (!mDownstream) {
		// For some testing, we might not have a radio connected.
		// That's OK, as long as we know it.
		LOG(WARN) << "XCCHL1Encoder with no radio, dumping frames";
		return;
	}

	for (int B=0; B<4; B++) {
		mBurst.time(mNextWriteTime);
		// Copy in the "encrypted" bits, GSM 05.03 4.1.5, 05.02 5.2.3.
		OBJLOG(DEEPDEBUG) << "XCCHL1Encoder mI["<<B<<"]=" << mI[B];
		mI[B].segment(0,57).copyToSegment(mBurst,3);
		mI[B].segment(57,57).copyToSegment(mBurst,88);
		// Send it to the radio.
		OBJLOG(DEEPDEBUG) << "XCCHL1Encoder mBurst=" << mBurst;
		mDownstream->writeHighSide(mBurst);
		rollForward();
	}
}

void SCHL1Encoder::writeHighSide(const L2Frame& frame)
{
	assert(mDownStream);

	resync();
	waitToSend();

	/* Only write 4 bytes, not the L2Frame garbage filler too! */
	BitVector vector(frame);
	vector.LSB8MSB();
	vector.copyToSegment(mD, 0, 32);

	// Generate the parity bits.
	mBlockCoder.writeParityWord(mD, mP);
	// Apply the convolutional encoder.
	mU.encode(mVCoder, mE);

	mE1.copyToSegment(mBurst, 3);
	mE2.copyToSegment(mBurst, 106);

	mBurst.time(mNextWriteTime);

	// Send to GSMTAP
	gWriteGSMTAP(ARFCN(), TN(), mNextWriteTime.FN(), typeAndOffset(), 
		false, false, mU, GSMTAP_BURST_SCH);

	mDownstream->writeHighSide(mBurst);

	rollForward();
}




void GeneratorL1Encoder::start()
{
	L1Encoder::start();
	mSendThread.start((void*(*)(void*))GeneratorL1EncoderServiceLoopAdapter,(void*)this);
}



void *GSM::GeneratorL1EncoderServiceLoopAdapter(GeneratorL1Encoder* gen)
{
	gen->serviceLoop();
	// DONTREACH
	return NULL;
}

void GeneratorL1Encoder::serviceLoop()
{
	while(mRunning)
	{
		if(mActive)
		{
			resync();
			waitToSend();
			generate();
		}
	}
}

FCCHL1Encoder::FCCHL1Encoder(L1FEC *wParent)
	:GeneratorL1Encoder(0,gFCCHMapping,wParent)
{
	mBurst.zero();
	mFillerBurst.zero();
}


void FCCHL1Encoder::generate()
{
	OBJLOG(DEEPDEBUG) << "FCCHL1Encoder " << mNextWriteTime;
	assert(mDownstream);
	resync();

	mBurst.time(mNextWriteTime);

	// Send to GSMTAP
	gWriteGSMTAP(ARFCN(), TN(), mNextWriteTime.FN(), typeAndOffset(), false,
		false, mBurst, GSMTAP_BURST_FCCH);

	mDownstream->writeHighSide(mBurst);
	rollForward();
}

TCHFACCHL1Decoder::TCHFACCHL1Decoder(
	unsigned wTN,
	const TDMAMapping& wMapping,
	L1FEC *wParent)
	:XCCHL1Decoder(wTN, wMapping, wParent),
	mTCHU(189),mTCHD(260),
	mClass1_c(mC.head(378)),mClass1A_d(mTCHD.head(50)),mClass2_c(mC.segment(378,78)),
	mTCHParity(0x0b,3,50)
{
	for (int i=0; i<8; i++) {
		mI[i] = SoftVector(114);
		// Fill with zeros just to make Valgrind happy.
		mI[i].fill(.0);
	}
}




void TCHFACCHL1Decoder::writeLowSide(const RxBurst& inBurst)
{
	OBJLOG(DEEPDEBUG) << "TCHFACCHL1Decoder " << inBurst;
	// If the channel is closed, ignore the burst.
	if (!active()) {
		OBJLOG(DEEPDEBUG) << "TCHFACCHL1Decoder not active, ignoring input";
		return;
	}

	// Send to GSMTAP
	gWriteGSMTAP(ARFCN(), inBurst.time().TN(), inBurst.time().FN(), 
		typeAndOffset(), false, true, mD, 0);

	processBurst(inBurst);
}





bool TCHFACCHL1Decoder::processBurst( const RxBurst& inBurst)
{
	// Accept the burst into the deinterleaving buffer.
	// Return true if we are ready to interleave.

	/* SACCH-like processing of RSSI and TimingError */
	// Get the physical parameters of the burst.
	// The actual phone settings change every 4 bursts,
	// so average over all 4.
	// RSSI is dB wrt full scale.
	mRSSI[mRSSICounter] = inBurst.RSSI();
	// Timing error is a float in symbol intervals.
	mTimingError[mRSSICounter] = inBurst.timingError();

	OBJLOG(DEBUG) << "TCHFACCHL1Decoder #" << mRSSICounter << ": RSSI=" << 
		inBurst.RSSI() << " timingError=" << inBurst.timingError();

	// This flag is used as a half-ass semaphore.
	// It is cleared when the new value is read.
	mPhyNew = true;
	mRSSICounter++;
	if (mRSSICounter>3) mRSSICounter=0;

	// TODO -- One quick test of burst validity is to look at the tail bits.
	// We could do that as a double-check against putting garbage into
	// the interleaver or accepting bad parameters.

	// The reverse index runs 0..7 as the bursts arrive.
	// It is the "B" index of GSM 05.03 3.1.3 and 3.1.4.
	int B = mMapping.reverseMapping(inBurst.time().FN()) % 8;
	// A negative value means that the demux is misconfigured.
	assert(B>=0);
	OBJLOG(DEEPDEBUG) << "TCHFACCHL1Decoder B=" << B << " " << inBurst;

	// Pull the data fields (e-bits) out of the burst and put them into i[B][].
	// GSM 05.03 3.1.4
	inBurst.data1().copyToSegment(mI[B],0);
	inBurst.data2().copyToSegment(mI[B],57);

	// Every 4th frame is the start of a new block.
	// So if this isn't a "4th" frame, return now.
	if (B%4!=3) return false;

	// Deinterleave according to the diagonal "phase" of B.
	// See GSM 05.03 3.1.3.
	// Deinterleaves i[] to c[]
	if (B==3) deinterleave(4);
	else deinterleave(0);

	// See if this was the end of a stolen frame, GSM 05.03 4.2.5.
	bool stolen = inBurst.Hl();
	OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder Hl=" << inBurst.Hl() << " Hu=" << inBurst.Hu();
	if (stolen) {
		if (decode()) {
			OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder good FACCH frame";
			countGoodFrame();
			mD.LSB8MSB();
			handleGoodFrame();
		} else {
			OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder bad FACCH frame";
			countBadFrame();
		}
	}

	// Always feed the traffic channel, even on a stolen frame.
	// decodeTCH will handle the GSM 06.11 bad frmae processing.
	bool traffic = decodeTCH(stolen);
	if (traffic) {
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder good TCH frame";
		countGoodFrame();
		// Don't let the channel timeout.
		mLock.lock();
		mT3109.set();
		mLock.unlock();
	}
	else countBadFrame();

	return true;
}




void TCHFACCHL1Decoder::deinterleave(int blockOffset )
{
	OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder blockOffset=" << blockOffset;
	for (int k=0; k<456; k++) {
		int B = ( k + blockOffset ) % 8;
		int j = 2*((49*k) % 57) + ((k%8)/4);
		mC[k] = mI[B][j];
		mI[B][j] = 0.5F;
	}
}





bool TCHFACCHL1Decoder::decodeTCH(bool stolen)
{
	// GSM 05.02 3.1.2, but backwards

	// If the frame wasn't stolen, we'll update this with parity later.
	bool good = !stolen;

	// Good or bad, we will be sending *something* to the speech channel.
	// Allocate it in this scope.
	unsigned char * newFrame = new unsigned char[33];

	if (!stolen) {

		// 3.1.2.2
		// decode from c[] to u[]
		mClass1_c.decode(mVCoder,mTCHU);
	
		// 3.1.2.2
		// copy class 2 bits c[] to d[]
		mClass2_c.sliced().copyToSegment(mTCHD,182);
	
		// 3.1.2.1
		// copy class 1 bits u[] to d[]
		for (unsigned k=0; k<=90; k++) {
			mTCHD[2*k] = mTCHU[k];
			mTCHD[2*k+1] = mTCHU[184-k];
		}
	
		// 3.1.2.1
		// check parity of class 1A
		unsigned sentParity = (~mTCHU.peekField(91,3)) & 0x07;
		unsigned calcParity = mClass1A_d.parity(mTCHParity) & 0x07;

		// 3.1.2.2
		// Check the tail bits, too.
		unsigned tail = mTCHU.peekField(185,4);
	
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder c[]=" << mC;
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder u[]=" << mTCHU;
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder d[]=" << mTCHD;
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Decoder sentParity=" << sentParity
			<< " calcParity=" << calcParity << " tail=" << tail;
		good = (sentParity==calcParity) && (tail==0);
		if (good) {
			// Undo Um's importance-sorted bit ordering.
			// See GSM 05.03 3.1 and Table 2.
			BitVector payload = mVFrame.payload();
			mTCHD.unmap(g610BitOrder,260,payload);
			mVFrame.pack(newFrame);
			// Save a copy for bad frame processing.
			memcpy(mPrevGoodFrame,newFrame,33);
		}
	}

	if (!good) {
		// Bad frame processing, GSM 06.11.
		// Attenuate block amplitudes and andomize grid positions.
		char rawByte = mPrevGoodFrame[27];
		unsigned xmaxc = rawByte & 0x01f;
		if (xmaxc>2) xmaxc -= 2;
		else xmaxc = 0;
		for (unsigned i=0; i<4; i++) {
			unsigned pos = random() % 4;
			mPrevGoodFrame[6+7*i] = (rawByte & 0x80) | pos | xmaxc;
			mPrevGoodFrame[7+7*i] &= 0x7F;
		}
		memcpy(newFrame,mPrevGoodFrame,33);
	}

	/* Only feed the speech channel if TCH, not FACCH */	
	if(!stolen)
	{
		assert(mUpstream);
		mUpstream->writeLowSideTCH(newFrame, mReadTime, RSSI(), TA(), mFER);
	}

	return good;
}








void GSM::TCHFACCHL1EncoderRoutine( TCHFACCHL1Encoder * encoder )
{
	while (encoder->active()) {
		encoder->dispatch();
	}
}




TCHFACCHL1Encoder::TCHFACCHL1Encoder(
	unsigned wTN,
	const TDMAMapping& wMapping,
	L1FEC *wParent)
	:XCCHL1Encoder(wTN, wMapping, wParent), 
	mPreviousFACCH(false),mOffset(0),
	mTCHU(189),mTCHD(260),
	mClass1_c(mC.head(378)),mClass1A_d(mTCHD.head(50)),mClass2_d(mTCHD.segment(182,78)),
	mTCHParity(0x0b,3,50)
{
	for(int k = 0; k<8; k++) {
		mI[k] = BitVector(114);
		// Fill with zeros just to make Valgrind happy.
		mI[k].fill(0);
	}
}




void TCHFACCHL1Encoder::start()
{
	L1Encoder::start();
	OBJLOG(DEBUG) <<"TCHFACCHL1Encoder";
	mEncoderThread.start((void*(*)(void*))TCHFACCHL1EncoderRoutine,(void*)this);
}




void TCHFACCHL1Encoder::open()
{
	// There was over stuff here at one time to justify overriding the default.
	// But it's gone now.
	XCCHL1Encoder::open();
}


void TCHFACCHL1Encoder::encodeTCH(const VocoderFrame& vFrame)
{	
	// GSM 05.02 3.1.2
	OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Encoder";

	// Reorder bits by importance.
	// See GSM 05.03 3.1 and Table 2.
	vFrame.payload().map(g610BitOrder,260,mTCHD);

	// 3.1.2.1 -- parity bits
	BitVector p = mTCHU.segment(91,3);
   	mTCHParity.writeParityWord(mClass1A_d,p);

	// 3.1.2.1 -- copy class 1 bits d[] to u[]
	for (unsigned k=0; k<=90; k++) {
		mTCHU[k] = mTCHD[2*k];
		mTCHU[184-k] = mTCHD[2*k+1];
	}

	// 3.1.2.1 -- tail bits in u[]
	// TODO -- This should only be needed once, in the constructor.
	for (unsigned k=185; k<=188; k++) mTCHU[k]=0;

	// 3.1.2.2 -- encode u[] to c[] for class 1
	mTCHU.encode(mVCoder,mClass1_c);

	// 3.1.2.2 -- copy class 2 d[] to c[]
	mClass2_d.copyToSegment(mC,378);

	// So the encoded speech frame is now in c[]
	// and ready for the interleaver.
}





void TCHFACCHL1Encoder::sendFrame( const L2Frame& frame )
{
	OBJLOG(DEEPDEBUG) << "TCHFACCHL1Encoder " << frame;
	mL2Q.write(new L2Frame(frame));
}



void TCHFACCHL1Encoder::dispatch()
{

	// No downstream?  That's a problem.
	assert(mDownstream);

	// Get right with the system clock.
	resync();

	// If the channel is not active, wait for a multiframe and return.
	// Most channels do not need this, becuase they are entirely data-driven
	// from above.  TCH/FACCH, however, must feed the interleaver on time.
	if (!active()) {
		mNextWriteTime += 26;
		gBTSL1.clock().wait(mNextWriteTime);
		return;
	}

	// Let previous data get transmitted.
	resync();
	waitToSend();
	
	// flag to control stealing bits
	bool currentFACCH = false; 
	
	// Speech latency control.
	// Since Asterisk is local, latency should be small.
	OBJLOG(INFO) <<"TCHFACCHL1Encoder speechQ.size=" << mSpeechQ.size();
	OBJLOG(INFO) <<"TCHFACCHL1Encoder L2Q.size=" << mL2Q.size();
	int maxQ = gConfig.getNum("GSM.MaxSpeechLatency");
	while (mSpeechQ.size() > maxQ) delete mSpeechQ.read();

	// Send, by priority: (1) FACCH, (2) TCH, (3) filler.
	if (L2Frame *fFrame = mL2Q.readNoBlock()) {
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Encoder FACCH " << *fFrame;
		currentFACCH = true;
		// Copy the L2 frame into u[] for processing.
		// GSM 05.03 4.1.1.
		fFrame->LSB8MSB();
		fFrame->copyTo(mU);
		// Encode u[] to c[], GSM 05.03 4.1.2 and 4.1.3.
		encode();
		delete fFrame;
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Encoder FACCH c[]=" << mC;
		// Flush the vocoder FIFO to limit latency.
		while (mSpeechQ.size()>0) delete mSpeechQ.read();
	} else if (VocoderFrame *tFrame = mSpeechQ.readNoBlock()) {
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Encoder TCH " << *tFrame;
		// Encode the speech frame into c[] as per GSM 05.03 3.1.2.
		encodeTCH(*tFrame);
		delete tFrame;
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Encoder TCH c[]=" << mC;
	} else {
		// We have no ready data but must send SOMETHING.
		// This filler pattern was captured from a Nokia 3310, BTW.
		static const BitVector fillerC("110100001000111100000000111001111101011100111101001111000000000000110111101111111110100110101010101010101010101010101010101010101010010000110000000000000000000000000000000000000000001101001111000000000000000000000000000000000000000000000000111010011010101010101010101010101010101010101010101001000011000000000000000000110100111100000000111001111101101000001100001101001111000000000000000000011001100000000000000000000000000000000000000000000000000000000001");
		fillerC.copyTo(mC);
		OBJLOG(DEEPDEBUG) <<"TCHFACCHL1Encoder filler FACCH=" << currentFACCH << " c[]=" << mC;
	}

	// Interleave c[] to i[].
	interleave(mOffset);

	// "mapping on a burst"
	// Map c[] into outgoing normal bursts, marking stealing flags as needed.
	// GMS 05.03 3.1.4.
	for (int B=0; B<4; B++) {
		// set TDMA position
		mBurst.time(mNextWriteTime);
		// copy in the bits
		mI[B+mOffset].segment(0,57).copyToSegment(mBurst,3);
		mI[B+mOffset].segment(57,57).copyToSegment(mBurst,88);
		// stealing bits
		mBurst.Hu(currentFACCH);
		mBurst.Hl(mPreviousFACCH);

		// Send to GSMTAP
		gWriteGSMTAP(ARFCN(), mBurst.time().TN(), mBurst.time().FN(), 
			typeAndOffset(), false, false, mD, 0);

		// send
		OBJLOG(DEEPDEBUG) <<"TCHFACCHEncoder sending burst=" << mBurst;
		mDownstream->writeHighSide(mBurst);
		rollForward();
	}	

	// Updaet the offset for the next transmission.
	if (mOffset==0) mOffset=4;
	else mOffset=0;

	// Save the stealing flag.
	mPreviousFACCH = currentFACCH;
}



void TCHFACCHL1Encoder::interleave(int blockOffset)
{
	// GSM 05.03, 3.1.3
	for (int k=0; k<456; k++) {
		int B = ( k + blockOffset ) % 8;
		int j = 2*((49*k) % 57) + ((k%8)/4);
		mI[B][j] = mC[k];
	}
}


bool TCHFACCHL1Decoder::uplinkLost() const
{
	mLock.lock();
	bool retVal = mT3109.expired();
	mLock.unlock();
	return retVal;
}



void SACCHL1FEC::setPhy(const SACCHL1FEC& other)
{
	mSACCHDecoder->setPhy(*other.mSACCHDecoder);
	mSACCHEncoder->setPhy(*other.mSACCHEncoder);
}

void SACCHL1FEC::setPhy(float RSSI, float timingError)
{
	mSACCHDecoder->setPhy(RSSI,timingError);
	mSACCHEncoder->setPhy(RSSI,timingError);
}




void SACCHL1Decoder::open()
{
	OBJLOG(DEBUG) << "SACCHL1Decoder";
	XCCHL1Decoder::open();
	// Set initial defaults for power and timing advance.
	// We know the handset sent the RACH burst at max power and 0 timing advance.
	mActualMSPower = 33;
	mActualMSTiming = 0;
	// Measured values should be set after opening with setPhy.
}



void SACCHL1Decoder::setPhy(float wRSSI, float wTimingError)
{
	// Used to initialize L1 phy parameters.
	for (int i=0; i<4; i++) mRSSI[i]=wRSSI;
	for (int i=0; i<4; i++) mTimingError[i]=wTimingError;
	mPhyNew = true;
	OBJLOG(INFO) << "SACCHL1Decoder RSSI=" << wRSSI << "timingError=" << wTimingError;
}

void SACCHL1Decoder::setPhy(const SACCHL1Decoder& other)
{
	// Used to initialize a new SACCH L1 phy parameters
	// from those of a preexisting established channel.
	mActualMSPower = other.mActualMSPower;
	mActualMSTiming = other.mActualMSTiming;
	for (int i=0; i<4; i++) mRSSI[i]=other.mRSSI[i];
	for (int i=0; i<4; i++) mTimingError[i]=other.mTimingError[i];
	mPhyNew = true;
	OBJLOG(INFO) << "SACCHL1Decoder actuals RSSI=" << mRSSI[0] << "timingError=" << mTimingError[0]
		<< " MSPower=" << mActualMSPower << " MSTiming=" << mActualMSTiming;
}



void SACCHL1Encoder::setPhy(float wRSSI, float wTimingError)
{
	// Used to initialize L1 phy parameters.
	// This is similar to the code for the closed loop tracking,
	// except that there's no damping.
	SACCHL1Decoder &sib = *SACCHSibling();
	// RSSI
	float RSSI = sib.RSSI();
	float RSSITarget = gConfig.getNum("GSM.RSSITarget");
	float deltaP = RSSI - RSSITarget;
	float actualPower = sib.actualMSPower();
	mOrderedMSPower = actualPower - deltaP;
	float maxPower = gConfig.getNum("GSM.MS.Power.Max");
	float minPower = gConfig.getNum("GSM.MS.Power.Min");
	if (mOrderedMSPower>maxPower) mOrderedMSPower=maxPower;
	else if (mOrderedMSPower<minPower) mOrderedMSPower=minPower;
	OBJLOG(INFO) <<"SACCHL1Encoder RSSI=" << RSSI << " target=" << RSSITarget
		<< " deltaP=" << deltaP << " actual=" << actualPower << " order=" << mOrderedMSPower;
	// Timing Advance
	float timingError = sib.timingError();
	float actualTiming = sib.actualMSTiming();
	mOrderedMSTiming = actualTiming + timingError;
	float maxTiming = gConfig.getNum("GSM.MS.TA.Max");
	if (mOrderedMSTiming<0.0F) mOrderedMSTiming=0.0F;
	else if (mOrderedMSTiming>maxTiming) mOrderedMSTiming=maxTiming;
	OBJLOG(INFO) << "SACCHL1Encoder timingError=" << timingError  <<
		" actual=" << actualTiming << " ordered=" << mOrderedMSTiming;
}


void SACCHL1Encoder::setPhy(const SACCHL1Encoder& other)
{
	// Used to initialize a new SACCH L1 phy parameters
	// from those of a preexisting established channel.
	mOrderedMSPower = other.mOrderedMSPower;
	mOrderedMSTiming = other.mOrderedMSTiming;
	OBJLOG(INFO) << "SACCHL1Encoder orders MSPower=" << mOrderedMSPower << " MSTiming=" << mOrderedMSTiming;
}





SACCHL1Encoder::SACCHL1Encoder( unsigned wTN, const TDMAMapping& wMapping, SACCHL1FEC *wParent)
	:XCCHL1Encoder(wTN,wMapping,(L1FEC*)wParent),
	mSACCHParent(wParent),
	mOrderedMSPower(33),mOrderedMSTiming(0)
{ }


void SACCHL1Encoder::open()
{
	OBJLOG(INFO) <<"SACCHL1Encoder";
	XCCHL1Encoder::open();
	mOrderedMSPower = 33;
	mOrderedMSTiming = 0;
}



SACCHL1Encoder* SACCHL1Decoder::SACCHSibling() 
{
	return mSACCHParent->encoder();
}

SACCHL1Decoder* SACCHL1Encoder::SACCHSibling() 
{
	return mSACCHParent->decoder();
}



void SACCHL1Encoder::sendFrame(const L2Frame& frame)
{
	OBJLOG(DEEPDEBUG) << "SACCHL1Encoder " << frame;

	// Physical header, GSM 04.04 6, 7.1
	// Power and timing control, GSM 05.08 4, GSM 05.10 5, 6.

	SACCHL1Decoder &sib = *SACCHSibling();
	if (sib.phyNew()) {
		// Power.  GSM 05.08 4.
		// Power expressed in dBm, RSSI in dB wrt max.
		float RSSI = sib.RSSI();
		float RSSITarget = gConfig.getNum("GSM.RSSITarget");
		float deltaP = RSSI - RSSITarget;
		float actualPower = sib.actualMSPower();
		float targetMSPower = actualPower - deltaP;
		float powerDamping = gConfig.getNum("GSM.MS.Power.Damping")*0.01F;
		mOrderedMSPower = powerDamping*mOrderedMSPower + (1.0F-powerDamping)*targetMSPower;
		float maxPower = gConfig.getNum("GSM.MS.Power.Max");
		float minPower = gConfig.getNum("GSM.MS.Power.Min");
		if (mOrderedMSPower>maxPower) mOrderedMSPower=maxPower;
		else if (mOrderedMSPower<minPower) mOrderedMSPower=minPower;
		OBJLOG(INFO) <<"SACCHL1Encoder RSSI=" << RSSI << " target=" << RSSITarget
			<< " deltaP=" << deltaP << " actual=" << actualPower << " order=" << mOrderedMSPower;
		// Timing.  GSM 05.10 5, 6.
		// Time expressed in symbol periods.
		float timingError = sib.timingError();
		float actualTiming = sib.actualMSTiming();
		float targetMSTiming = actualTiming + timingError;
		float TADamping = gConfig.getNum("GSM.MS.TA.Damping")*0.01F;
		mOrderedMSTiming = TADamping*mOrderedMSTiming + (1.0F-TADamping)*targetMSTiming;
		float maxTiming = gConfig.getNum("GSM.MS.TA.Max");
		if (mOrderedMSTiming<0.0F) mOrderedMSTiming=0.0F;
		else if (mOrderedMSTiming>maxTiming) mOrderedMSTiming=maxTiming;
		OBJLOG(INFO) << "SACCHL1Encoder timingError=" << timingError
			<< " actual=" << actualTiming << " ordered=" << mOrderedMSTiming
			<< " target=" << targetMSTiming;
	}

	// Write physical header into mU and then call base class.

	// SACCH physical header, GSM 04.04 6.1, 7.1.
	OBJLOG(DEBUG) <<"SACCHL1Encoder orders pow=" << mOrderedMSPower << " TA=" << mOrderedMSTiming;
	mU.fillField(0,encodePower(mOrderedMSPower),8);
	mU.fillField(8,(int)(mOrderedMSTiming+0.5F),8);	// timing (GSM 04.04 6.1)
	OBJLOG(DEBUG) << "SACCHL1Encoder phy header " << mU.head(16);

	// Encode the rest of the frame.
	XCCHL1Encoder::sendFrame(frame);
}



// vim: ts=4 sw=4
