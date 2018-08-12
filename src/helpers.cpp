#include "helpers.h"
#include <string>
#include <stdexcept>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>

#define MOD_GZIP_ZLIB_WINDOWSIZE 15
#define MOD_GZIP_ZLIB_CFACTOR 9
#define MOD_GZIP_ZLIB_BSIZE 8096

using namespace ClipperLib;
namespace geom = boost::geometry;

// zlib routines from http://panthema.net/2007/0328-ZLibString.html

// Compress a STL string using zlib with given compression level, and return the binary data
std::string compress_string(const std::string& str,
                            int compressionlevel,
                            bool asGzip) {
    z_stream zs;                        // z_stream is zlib's control structure
    memset(&zs, 0, sizeof(zs));

	if (asGzip) {
		if (deflateInit2(&zs, compressionlevel, Z_DEFLATED,
		                MOD_GZIP_ZLIB_WINDOWSIZE + 16, MOD_GZIP_ZLIB_CFACTOR, Z_DEFAULT_STRATEGY) != Z_OK)
	        throw(std::runtime_error("deflateInit2 failed while compressing."));
	} else {
	    if (deflateInit(&zs, compressionlevel) != Z_OK)
	        throw(std::runtime_error("deflateInit failed while compressing."));
	}

    zs.next_in = (Bytef*)str.data();
    zs.avail_in = str.size();           // set the z_stream's input

    int ret;
    char outbuffer[32768];
    std::string outstring;

    // retrieve the compressed bytes blockwise
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = deflate(&zs, Z_FINISH);

        if (outstring.size() < zs.total_out) {
            // append the block to the output string
            outstring.append(outbuffer,
                             zs.total_out - outstring.size());
        }
    } while (ret == Z_OK);

    deflateEnd(&zs);

    if (ret != Z_STREAM_END) {          // an error occurred that was not EOF
        std::ostringstream oss;
        oss << "Exception during zlib compression: (" << ret << ") " << zs.msg;
        throw(std::runtime_error(oss.str()));
    }

    return outstring;
}

// Decompress an STL string using zlib and return the original data.
std::string decompress_string(const std::string& str) {
    z_stream zs;                        // z_stream is zlib's control structure
    memset(&zs, 0, sizeof(zs));

    if (inflateInit(&zs) != Z_OK)
        throw(std::runtime_error("inflateInit failed while decompressing."));

    zs.next_in = (Bytef*)str.data();
    zs.avail_in = str.size();

    int ret;
    char outbuffer[32768];
    std::string outstring;

    // get the decompressed bytes blockwise using repeated calls to inflate
    do {
        zs.next_out = reinterpret_cast<Bytef*>(outbuffer);
        zs.avail_out = sizeof(outbuffer);

        ret = inflate(&zs, 0);

        if (outstring.size() < zs.total_out) {
            outstring.append(outbuffer,
                             zs.total_out - outstring.size());
        }

    } while (ret == Z_OK);

    inflateEnd(&zs);

    if (ret != Z_STREAM_END) {          // an error occurred that was not EOF
        std::ostringstream oss;
        oss << "Exception during zlib decompression: (" << ret << ") "
            << zs.msg;
        throw(std::runtime_error(oss.str()));
    }

    return outstring;
}

const double CLIPPER_SCALE = 1e8;

void ConvertToClipper(const Polygon &p, Path &outer, Paths &inners)
{
	outer.clear();
	inners.clear();
	const Polygon::ring_type &out = p.outer();
	const Polygon::inner_container_type &inns = p.inners();

	for(size_t i=0; i<out.size(); i++)
	{
		const Point &pt = out[i];
		outer.push_back(IntPoint(std::round(pt.x() * CLIPPER_SCALE), std::round(pt.y() * CLIPPER_SCALE)));
	}

	for(size_t i=0; i<inns.size(); i++)
	{
		Path in;
		const Polygon::ring_type &inner = inns[i];
		for(size_t j=0; j<inner.size(); j++)
		{
			const Point &pt = inner[j];
			in.push_back(IntPoint(std::round(pt.x() * CLIPPER_SCALE), std::round(pt.y() * CLIPPER_SCALE)));
		}
		inners.push_back(in);
	}
}

void ConvertToClipper(const MultiPolygon &mp, Paths &out)
{
	// Convert boost geometries to clipper paths 
	out.clear();
	Clipper c;
	c.StrictlySimple(true);
	for(size_t i=0; i<mp.size(); i++)
	{
		const Polygon &p = mp[i];
		Path outer;
		Paths inners;
		ConvertToClipper(p, outer, inners);

		// For polygons with holes,
		if(inners.size()>0)
		{
			// Find polygon shapes without needing holes
			Paths simp;
			c.AddPath(outer, ptSubject, true);
			c.AddPaths(inners, ptClip, true);
			c.Execute(ctDifference, simp, pftEvenOdd, pftEvenOdd);
			c.Clear();

			out.insert(out.end(), simp.begin(), simp.end());
		}
		else
		{
			out.push_back(outer);				
		}
	}
}

void ConvertToClipper(const MultiPolygon &mp, PolyTree &out)
{
	// Convert boost geometries to clipper paths 
	out.Clear();
	Clipper c;
	c.StrictlySimple(true);

	Paths outers;
	Paths inners;
	for(size_t i=0; i<mp.size(); i++)
	{
		const Polygon &p = mp[i];
		Path outerTmp;
		Paths innersTmp;
		ConvertToClipper(p, outerTmp, innersTmp);
		outers.push_back(outerTmp);
		inners.insert(inners.end(), innersTmp.begin(), innersTmp.end());
	}

	// Find polygon shapes
	c.AddPaths(outers, ptSubject, true);
	c.AddPaths(inners, ptClip, true);
	c.Execute(ctDifference, out, pftEvenOdd, pftEvenOdd);
	c.Clear();
}

void ConvertFromClipper(const Path &outer, const Paths &inners, Polygon &p)
{
	p.clear();
	Polygon::ring_type &out = p.outer();
	Polygon::inner_container_type &inns = p.inners();
	
	for(size_t i=0; i<outer.size(); i++)
	{
		const IntPoint &pt = outer[i];
		out.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
	}
	if(outer.size()>0)
	{
		//Start point in ring is repeated
		const IntPoint &pt = outer[0];
		out.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
	}

	for(size_t i=0; i<inners.size(); i++)
	{
		const Path &inn = inners[i];
		Polygon::ring_type inn2;
		for(size_t j=0; j<inn.size(); j++)
		{
			const IntPoint &pt = inn[j];
			inn2.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
		}
		if(inn.size()>0)
		{
			//Start point in ring is repeated
			const IntPoint &pt = inn[0];
			inn2.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
		}
		inns.push_back(inn2);
	}
	//Fix orientation of rings
	geom::correct(p);
}

void ConvertFromClipper(const Paths &polys, MultiPolygon &mp)
{
	mp.clear();

	for(size_t pn=0; pn < polys.size(); pn++)
	{
		const Path &pth = polys[pn];
		Polygon p;
		Polygon::ring_type &otr = p.outer();

		for(size_t i=0; i<pth.size(); i++)
		{
			const IntPoint &pt = pth[i];
			otr.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
		}
		if(pth.size()>0)
		{
			//Start point in ring is repeated
			const IntPoint &pt = pth[0];
			otr.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
		}

		//Fix orientation of rings
		geom::correct(p);
		mp.push_back(p);
	}
	assert(polys.size() == mp.size());
}

void ConvertFromClipper(const ClipperLib::PolyTree &pt, MultiPolygon &mp)
{
	mp.clear();

	PolyNode *cursor = pt.GetFirst();
	while(cursor != nullptr)
	{
		Polygon p;
		Polygon::ring_type &otr = p.outer();
		Polygon::inner_container_type &inns = p.inners();

		for(size_t i=0; i<cursor->Contour.size(); i++)
		{
			const IntPoint &pt = cursor->Contour[i];
			otr.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
		}
		if(cursor->Contour.size()>0)
		{
			//Start point in ring is repeated
			const IntPoint &pt = cursor->Contour[0];
			otr.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
		}

		for(size_t i=0; i<cursor->Childs.size(); i++)
		{
			const Path &inn = cursor->Childs[i]->Contour;
			Polygon::ring_type inn2;
			for(size_t j=0; j<inn.size(); j++)
			{
				const IntPoint &pt = inn[j];
				inn2.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
			}
			if(inn.size()>0)
			{
				//Start point in ring is repeated
				const IntPoint &pt = inn[0];
				inn2.push_back(Point(pt.X / CLIPPER_SCALE, pt.Y / CLIPPER_SCALE));
			}
			inns.push_back(inn2);

			//FIXME what about children of children? i.e. nested polygons within holes
		}

		//Fix orientation of rings
		geom::correct(p);
		mp.push_back(p);

		cursor = cursor->GetNext();
	}

}
