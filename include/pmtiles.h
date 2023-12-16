/*! \file */ 
#ifndef _PMTILES_H
#define _PMTILES_H

class PMTiles { 
//	sqlite::database db;
//	std::vector<sqlite::database_binder> preparedStatements;
//	std::mutex m;
//	bool inTransaction;

//	std::shared_ptr<std::vector<PendingStatement>> pendingStatements1, pendingStatements2;
//	std::mutex pendingStatementsMutex;

//	void insertOrReplace(int zoom, int x, int y, const std::string& data, bool isMerge);
//	void flushPendingStatements();

public:
	PMTiles();
	virtual ~PMTiles();
//	void openForWriting(std::string &filename);
//	void writeMetadata(std::string key, std::string value);
//	void saveTile(int zoom, int x, int y, std::string *data, bool isMerge);
//	void closeForWriting();
};

#endif //_PMTILES_H

