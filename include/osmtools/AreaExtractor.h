#ifndef OSM_TOOLS_AREA_EXTRACTOR_H
#define OSM_TOOLS_AREA_EXTRACTOR_H
#include <string>
#include <cstdlib>
#include <sserialize/spatial/GeoMultiPolygon.h>
#include <sserialize/spatial/GeoRegionStore.h>
#include <osmpbf/pbistream.h>
#include <osmpbf/primitiveblockinputadaptor.h>
#include <osmpbf/irelation.h>
#include <osmpbf/iway.h>
#include <osmpbf/inode.h>
#include <osmtools/AreaExtractorFilters.h>
#include <osmpbf/parsehelpers.h>


/** This is a simple class to extract areas from OpenStreetMap pbf-files
  * It needs the osmpbf and sserialize libraries in global include paths
  */

namespace osmtools {
namespace detail {
namespace AreaExtractor {

struct MultiPolyResolver {
	typedef std::vector<int64_t> RawWay;
	
	struct RawMultiPoly {
		RawWay innerWays;
		RawWay outerWays;
	};
	
	static bool closedPolysFromWays(const std::vector<RawWay> & ways, std::vector<RawWay> & closedWays);
	static bool multiPolyFromWays(const std::vector<RawWay> & innerIn, const std::vector<RawWay> & outerIn, std::vector<RawWay> & innerOut, std::vector<RawWay> & outerOut);
	
	///@param mapper maps from node-id to sserialize::spatial::GeoPoint @return either a GeoMultiPolygon or a GeoPolygon
	template<typename T_ID_GEO_MAPPER>
	static sserialize::spatial::GeoRegion * multiPolyFromClosedWays(const std::vector<RawWay> & inner, const std::vector<RawWay> & outer, const T_ID_GEO_MAPPER & mapper) {
		sserialize::spatial::GeoMultiPolygon * mupoptr = new sserialize::spatial::GeoMultiPolygon();
		sserialize::spatial::GeoMultiPolygon & mupo = *mupoptr;
		for(std::vector<RawWay>::const_iterator polyit(inner.begin()), end(inner.end()); polyit != end; ++polyit) {
			mupo.innerPolygons().resize(mupo.innerPolygons().size()+1);
			sserialize::remap(*polyit, mupo.innerPolygons().back().points(), mapper);
		}
		for(std::vector<RawWay>::const_iterator polyit(outer.begin()), end(outer.end()); polyit != end; ++polyit) {
			mupo.outerPolygons().resize(mupo.outerPolygons().size()+1);
			sserialize::remap(*polyit, mupo.outerPolygons().back().points(), mapper);
		}
		if (mupoptr->innerPolygons().size() == 0 && mupoptr->outerPolygons().size() == 1) {
			using std::swap;
			sserialize::spatial::GeoPolygon * gpo = new sserialize::spatial::GeoPolygon();
			swap(*gpo, mupoptr->outerPolygons().front());
			gpo->recalculateBoundary();
			delete mupoptr;
			return gpo;
		}
		mupo.recalculateBoundary();
		return mupoptr;
	}
	
	///@param mapper maps from node-id to sserialize::spatial::GeoPoint
	template<typename T_ID_GEO_MAPPER>
	static sserialize::spatial::GeoMultiPolygon multiPolyFromClosedWays(const RawMultiPoly & p, const T_ID_GEO_MAPPER & mapper) {
		return multiPolyFromClosedWays(p.innerWays, p.outerWays, mapper);
	}
	
};

}}//end namespace detail::AreaExtractor

class AreaExtractor: public detail::AreaExtractor::Base {
public:
	struct ValueType {
		std::unordered_map<std::string, std::string> kv;
		std::shared_ptr<sserialize::spatial::GeoRegion> * region;
	};
private:
	typedef std::pair<double, double> Point;
	struct Context {
		std::unordered_map<int64_t, detail::AreaExtractor::MultiPolyResolver::RawWay > rawWays;
		std::mutex rawWaysLock;
		
		std::unordered_map<int64_t, Point> nodes;
		std::mutex nodesLock;
		
		osmpbf::PbiStream & inFile;
		ExtractionTypes extractionTypes;
		bool needsName{false};
		bool verbose{false};
		bool snapGeometry{false};
		generics::RCPtr<osmpbf::AbstractTagFilter> externalFilter;
		sserialize::ProgressInfo pinfo;
		
		std::atomic<uint32_t> relevantWaysSize;
		std::atomic<uint32_t> relevantRelationsSize;
		
		std::atomic<uint32_t> assembledRelevantWays;
		std::atomic<uint32_t> assembledRelevantRelations;
		
		Context(osmpbf::PbiStream & inFile) : inFile(inFile), relevantWaysSize(0), relevantRelationsSize(0), assembledRelevantWays(0), assembledRelevantRelations(0) {}
	};
	struct ExtractorCallBack {
		virtual void operator()(const std::shared_ptr<sserialize::spatial::GeoRegion> & region, osmpbf::IPrimitive & primitive) = 0;
	};
	struct ExtractionFunctorBase {
		Context * ctx;
		osmpbf::PrimitiveBlockInputAdaptor * pbi;
		generics::RCPtr<osmpbf::AbstractTagFilter> mainFilter;
		void assignInputAdaptor(osmpbf::PrimitiveBlockInputAdaptor & pbi);
		ExtractionFunctorBase(Context * ctx);
		ExtractionFunctorBase(const ExtractionFunctorBase & other);
		~ExtractionFunctorBase() {}
	};
	//Private processor
	struct RelationWaysExtractor: ExtractionFunctorBase {
		std::unordered_set<int64_t> myWayIds;
		RelationWaysExtractor(Context * ctx) : ExtractionFunctorBase(ctx) {}
		RelationWaysExtractor(const RelationWaysExtractor & o) : ExtractionFunctorBase(o) {}
		void operator()(osmpbf::PrimitiveBlockInputAdaptor & pbi);
	};
	//Private processor
	struct WayRefsExtractor: ExtractionFunctorBase {
		std::unordered_set<int64_t> myRefs;
		WayRefsExtractor(Context * ctx) : ExtractionFunctorBase(ctx) {}
		WayRefsExtractor(const WayRefsExtractor & o) : ExtractionFunctorBase(o) {}
		void operator()(osmpbf::PrimitiveBlockInputAdaptor & pbi);
	};
	//Private processor
	struct RelationWayNodeRefsExtractor {
		Context * ctx;
		std::unordered_set<int64_t> myWayRefs;
		RelationWayNodeRefsExtractor(Context * ctx) : ctx(ctx) {}
		RelationWayNodeRefsExtractor(const RelationWayNodeRefsExtractor & o) : ctx(o.ctx) {}
		void operator()(osmpbf::PrimitiveBlockInputAdaptor & pbi);
	};
	//Private processor
	struct WayExtractor: ExtractionFunctorBase {
		ExtractorCallBack * cb;
		WayExtractor(Context * ctx, ExtractorCallBack * cb) : ExtractionFunctorBase(ctx), cb(cb) {}
		WayExtractor(const WayExtractor & o) : ExtractionFunctorBase(o), cb(o.cb) {}
		void operator()(osmpbf::PrimitiveBlockInputAdaptor & pbi);
	};
	//Private processor
	struct RelationExtractor: ExtractionFunctorBase {
		ExtractorCallBack * cb;
		RelationExtractor(Context * ctx, ExtractorCallBack * cb) : ExtractionFunctorBase(ctx), cb(cb) {}
		RelationExtractor(const RelationExtractor & o) : ExtractionFunctorBase(o), cb(o.cb) {}
		void operator()(osmpbf::PrimitiveBlockInputAdaptor & pbi);
	};
	//No private processor
	struct NodeGatherer {
		Context * ctx;
		NodeGatherer(Context * ctx) : ctx(ctx) {}
		NodeGatherer(const NodeGatherer & o) : ctx(o.ctx) {}
		void operator()(osmpbf::PrimitiveBlockInputAdaptor & pbi);
	};
public:
	AreaExtractor(bool verbose = false, bool snapGeometry = false) : m_verbose(verbose), m_snapGeometry(snapGeometry) {}
	virtual ~AreaExtractor() {}
	///@param processor (const std::shared_ptr<sserialize::spatial::GeoRegion> & region, osmpbf::IPrimitive & primitive), MUST be thread-safe
	///@param filter additional AND-filter 
	///@param numThreads number of threads, 0 for auto-detecting
	template<typename TProcessor>
	bool extract(const std::string & inputFileName, const TProcessor & processor, ExtractionTypes extractionTypes = ET_ALL_SPECIAL_BUT_BUILDINGS, const generics::RCPtr<osmpbf::AbstractTagFilter> & filter = generics::RCPtr<osmpbf::AbstractTagFilter>(), uint32_t numThreads = 0, const std::string & msg = std::string("AreaExtractor"));

	template<typename TProcessor>
	bool extract(osmpbf::PbiStream & inData, TProcessor processor, ExtractionTypes extractionTypes = ET_ALL_SPECIAL_BUT_BUILDINGS, const generics::RCPtr<osmpbf::AbstractTagFilter> & filter = generics::RCPtr<osmpbf::AbstractTagFilter>(), uint32_t numThreads = 0, const std::string & msg = std::string("AreaExtractor"));
private:
	bool m_verbose;
	bool m_snapGeometry;
};

template<typename TProcessor>
bool 
AreaExtractor::extract(
	const std::string & inputFileName,
	const TProcessor & processor,
	ExtractionTypes extractionTypes,
	const generics::RCPtr<osmpbf::AbstractTagFilter> & filter, uint32_t numThreads,
	const std::string & msg)
{
	osmpbf::OSMFileIn inFile(inputFileName);
	if (!inFile.open()) {
		std::cout << "Failed to open " <<  inputFileName << std::endl;
		return false;
	}
	osmpbf::PbiStream is(std::move(inFile));
	return AreaExtractor::extract<TProcessor>(is, processor, extractionTypes, filter, numThreads, msg);
}


template<typename TProcessor>
bool AreaExtractor::extract(osmpbf::PbiStream & inData, TProcessor processor, ExtractionTypes extractionTypes, const generics::RCPtr<osmpbf::AbstractTagFilter> & filter, uint32_t numThreads, const std::string & msg) {
	if (! (extractionTypes & (ET_ALL_SPECIAL | ET_ALL_MULTIPOLYGONS))) {
		return false;
	}
	
	Context ctx(inData);
	ctx.extractionTypes = extractionTypes;
	ctx.externalFilter = filter;
	ctx.verbose = m_verbose;
	ctx.snapGeometry = m_snapGeometry;
	
	struct MyCB: ExtractorCallBack {
		TProcessor * processor;
		virtual void operator()(const std::shared_ptr< sserialize::spatial::GeoRegion >& region, osmpbf::IPrimitive& primitive) {
			(*processor)(region, primitive);
		}
	} cb;
	cb.processor = &processor;
	
	NodeGatherer ng(&ctx);
	
	if (extractionTypes & ET_PRIMITIVE_WAYS) {
		WayRefsExtractor wre(&ctx);
		WayExtractor we(&ctx, &cb);

		ctx.pinfo.begin(ctx.inFile.dataSize(), msg + ": Ways' node-refs");
		ctx.inFile.reset();
		osmpbf::parseFileCPPThreads(ctx.inFile, wre, numThreads, 1, true);
		ctx.pinfo.end();
		
		std::cout << msg << ": Found " << ctx.relevantWaysSize << " ways\n";
		std::cout << msg << ": Need to fetch " << ctx.nodes.size() << " nodes" << std::endl;

		ctx.pinfo.begin(ctx.inFile.dataSize(), msg + ": Ways' nodes");
		ctx.inFile.reset();
		osmpbf::parseFileCPPThreads(ctx.inFile, ng, numThreads, 1, false);
		ctx.pinfo.end();
		
		ctx.pinfo.begin(ctx.inFile.dataSize(), msg + ": Assembling ways");
		ctx.inFile.reset();
		osmpbf::parseFileCPPThreads(ctx.inFile, WayExtractor(&ctx, &cb), numThreads, 1, true);
		ctx.pinfo.end();
	}
	
	ctx.nodes.clear();
	if (extractionTypes & ET_PRIMITIVE_RELATIONS) {
		RelationWaysExtractor rwe(&ctx);
		RelationWayNodeRefsExtractor rwnr(&ctx);
		RelationExtractor re(&ctx, &cb);

		ctx.pinfo.begin(ctx.inFile.dataSize(), msg + ": Relation's ways");
		ctx.inFile.reset();
		osmpbf::parseFileCPPThreads(ctx.inFile, rwe, numThreads, 1, true);
		ctx.pinfo.end();

		std::cout << msg << ": Found " << ctx.relevantRelationsSize << " relations with " << ctx.rawWays.size() << " ways \n";
		
		ctx.pinfo.begin(ctx.inFile.dataSize(), msg + ": Relation-ways' node-refs");
		ctx.inFile.reset();
		osmpbf::parseFileCPPThreads(ctx.inFile, rwnr, numThreads, 1, true);
		ctx.pinfo.end();
		
		std::cout << msg << ": Need to fetch " << ctx.nodes.size() << " nodes" << std::endl;
		
		ctx.pinfo.begin(ctx.inFile.dataSize(), msg + ": Relation-ways' nodes");
		ctx.inFile.reset();
		osmpbf::parseFileCPPThreads(ctx.inFile, ng, numThreads, 1, false);
		ctx.pinfo.end();
		
		ctx.pinfo.begin(ctx.inFile.dataSize(), msg + ": Assembling relations");
		ctx.inFile.reset();
		osmpbf::parseFileCPPThreads(ctx.inFile, re, numThreads, 1, true);
		ctx.pinfo.end();
	}
	ctx.nodes.clear();

	std::cout << msg << ": Assembled " << ctx.assembledRelevantWays << "/" << ctx.relevantWaysSize << " ways and " << ctx.assembledRelevantRelations << "/" << ctx.relevantRelationsSize << " relations" << std::endl;
	
	return true;
}

}//end namespace

#endif
