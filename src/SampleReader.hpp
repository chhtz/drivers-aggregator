#ifndef __AGGREGATOR_HPP__
#define __AGGREGATOR_HPP__

#include <base/time.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <boost/function.hpp>
#include <boost/tuple/tuple.hpp>
#include <stdexcept> 
#include <iostream>

namespace aggregator {

    class SampleReader
    {
	class StreamBase
	{
	    public:
		StreamBase() : overdue(false) {}
		bool overdue;
		virtual void pop(bool late) = 0;
		virtual bool hasData() const = 0;
		virtual base::Time nextTimeStamp() const = 0;
		virtual std::pair<size_t, size_t> getBufferStatus() const = 0;

		friend std::ostream &operator<<(std::ostream &stream, const aggregator::SampleReader::StreamBase &base);
	};

	template <class T> class Stream : public StreamBase
	{
	    typedef std::pair<base::Time,T> item;
	    std::queue<item> buffer;
	    size_t bufferSize;
	    boost::function<void (base::Time ts, T value)> callback;
	    base::Time period; 
	    base::Time lastTime;

	public:
	    Stream( boost::function<void (base::Time ts, T value)> callback, size_t bufferSize = 10, base::Time period = base::Time() )
		: bufferSize( bufferSize ), callback(callback), period(period) {}

	    virtual std::pair<size_t, size_t> getBufferStatus() const
	    {
		return std::make_pair( buffer.size(), bufferSize );
	    }

	    void push( base::Time ts, T data ) 
	    { 
		if( ts < lastTime )
		    return;

		lastTime = ts;

		// if the buffer is full, make some space
		// sorry old data, you gotta go!
		while( bufferSize > 0 && buffer.size() >= bufferSize )
		{
		    buffer.pop();
		}

		buffer.push( std::make_pair(ts, data) ); 
	    }

	    /** take the last item of the stream queue and 
	     * call the callback in case the data is not late
	     */
	    void pop( bool late = false ) 
	    { 
		if( hasData() )
		{
		    if( !late ) {
			overdue = false;
			callback( buffer.front().first, buffer.front().second );
		    }

		    buffer.pop();
		}
	    }

	    bool hasData() const
	    { return !buffer.empty(); }

	    base::Time nextTimeStamp() const
	    {
		if( hasData() )
		    return buffer.front().first;
		else 
		    return lastTime + period;
	    }
	};

	struct StreamIndex
	{
	    StreamIndex( const base::Time &time, bool hasData, StreamBase* stream)
		: time(time), hasData(hasData), stream(stream) {}

	    bool operator <(const StreamIndex &other) const { return time < other.time; }

	    base::Time time;
	    bool hasData;
	    StreamBase* stream;
	};


	typedef std::vector<StreamBase*> stream_vector;
	stream_vector streams;
	base::Time timeout;

	/** time of the last sample that came in */
	base::Time latest_ts;

	/** time of the last sample that went out */
	base::Time current_ts;

    public:
	explicit SampleReader(base::Time timeout = base::Time(1))
	    : timeout(timeout) {}

	~SampleReader()
	{
	    for(stream_vector::iterator it=streams.begin();it != streams.end();it++)
		delete *it;
	}

	/** Set the time the Estimator will wait for an expected reading on any of the streams.
	 * This number effectively puts an upper limit to the lag that can be created due to 
	 * delay or missing values on the channels.
	 */
	void setTimeout( base::Time t )
	{
	    timeout = t;
	}

	/** Will register a stream with the aggregator.
	 *
	 * @param callback - will be called for data gone through the synchronization process
	 * @param bufferSize - The size of the internal FIFO buffer. This should be at least 
	 *  	the amount of samples that can occur in a timeout period.
	 * @param period - time between sensor readings. This will be used to estimate when the 
	 *	next reading should arrive, so out of order arrivals are possible. Set to 0 if not a periodic stream
	 *
	 * @result - stream index, which is used to identify the stream (e.g. for push).
	 */
	template <class T> int registerStream( boost::function<void (base::Time ts, T value)> callback, int bufferSize = 10, base::Time period = base::Time() ) 
	{
	    streams.push_back( new Stream<T>(callback, bufferSize, period) );
	    return streams.size() - 1;
	}

	/** Push new data into the stream
	 * @param ts - the timestamp of the data item
	 * @param data - the data added to the stream
	 */
	template <class T> void push( int idx, base::Time ts, const T& data )
	{
	    if( idx < 0 )
		throw std::runtime_error("invalid stream index.");

	    // if the timestamp of the data item comming in is older than the current time - timeout, we don't even put it into the queue
	    if( (ts + timeout) < latest_ts )
	    {
		return;
	    }

	    if( ts > latest_ts )
		latest_ts = ts;

	    Stream<T>* stream = dynamic_cast<Stream<T>*>(streams[idx]);
	    assert( stream );

	    stream->push( ts, data );
	}

	/** This will go through the available streams and look for the
	 * oldest available data. The data can be either existing are predicted
	 * through the period. 
	 * 
	 * There are three different cases that can happen:
	 *  - The data is already available. In this case that data is forwarded
	 *    to the callback.
	 *  - The data is not yet available, and the time difference between oldest
	 *    data and newest data is below the timeout threshold. In this case
	 *    no data is called.
	 *  - The data is not yet available, and the timeout is reached. In this
	 *    case, the oldest data (which is obviously non-available) is ignored,
	 *    and only newer data is considered.
	 *
	 *  @result - true if there is more available data in any of the streams
	 */
	bool step()
	{
	    if( streams.empty() )
		return false;

	    std::vector<StreamIndex> items;
	    bool listHasData = false;
	    for(stream_vector::iterator it=streams.begin();it != streams.end();it++)
	    {
		bool hasData, late;

		// throw out late data
		do {
		    base::Time ts = (*it)->nextTimeStamp();
		    late = (ts < current_ts);
		    hasData = (*it)->hasData();
		    if( late && hasData )
		    {
			(*it)->pop(late);
		    }
		    else if( hasData || !ts.isNull() )
		    {
			// stream is only considered if it either has data,
			// or is expecting data
			items.push_back( StreamIndex(
				    ts,
				    hasData,
				    *it ) );

			// mark if we have any real data
			listHasData |= hasData;
		    }
		} while( late && hasData );

	    }

	    // return false if we just don't have any data
	    if( items.empty() || !listHasData )
		return false;

	    // sort for timestamp
	    std::sort(items.begin(), items.end());

	    for(std::vector<StreamIndex>::iterator it=items.begin();it != items.end();it++)
	    {
		if( it->hasData ) 
		{
		    // if stream has current data, pop that data
		    (it->stream)->pop(false);
		    current_ts = it->time;
		    return true;
		}
		else if( (it->time + timeout) > latest_ts )
		{
		    // if there is no data, but the expected data has
		    // not run out yet, wait for it.
		    return false;
		}
		else
		{
		    // mark this stream as overdue
		    (it->stream)->overdue = true;
		}
	    }

	    return false;
	}

	/** latency is the time difference between the latest data item that
	 * has come in, and the latest data item that went out
	 */
	base::Time getLatency() const { return latest_ts - current_ts; };

	/** return the time of the last data item that went out
	 */
	base::Time getCurrentTime() const { return current_ts; };

	/** return the time of the last data item that came in
	 */
	base::Time getLatestTime() const { return latest_ts; }

	/** return the buffer status as a std::pair. first element in pair is
	 * the current buffer fill and the second element is the buffer size 
	 */
	std::pair<size_t, size_t> getBufferStatus(int idx) const
	{
	    return streams[idx]->getBufferStatus();
	}

	friend std::ostream &operator<<(std::ostream &stream, const aggregator::SampleReader::StreamBase &base);
	friend std::ostream &operator<<(std::ostream &stream, const aggregator::SampleReader &re);
    };

    std::ostream &operator<<(std::ostream &stream, const aggregator::SampleReader &re)
    {
	using ::operator <<;
	stream << "current time: " << re.getCurrentTime() << " latest time:" << re.getLatestTime() << " latency: " << re.getLatency() << std::endl;
	for(size_t i=0;i<re.streams.size();i++)
	{
	    stream << i << ":" << *re.streams[i] << std::endl;
	}

	return stream;
    }

    std::ostream &operator<<(std::ostream &stream, const aggregator::SampleReader::StreamBase &base)
    {
	using ::operator <<;
	const std::pair<size_t, size_t> &status( base.getBufferStatus() );
	stream << status.first << "\t" << status.second << "\t" << base.overdue << "\t" << base.nextTimeStamp();
	return stream;
    }
}

#endif
