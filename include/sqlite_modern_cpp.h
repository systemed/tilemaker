#pragma once
#include<string>
#include<stdexcept>
#include"sqlite3.h"

/*
	This is an earlier version of sqlite_modern_cpp (current versions break on OS X), lightly patched
	to include a database.init() method, and to use the && operator for inserting blobs.
	-- Richard Fairhurst, 28.06.2015
	Very simplified write-only version
	-- Shunsuke Shimizu, 22.04.2016
*/

namespace sqlite {

	class database;
	class database_binder;

	class database_binder {
	private:
		sqlite3 * _db;
		std::u16string _sql;
		sqlite3_stmt* _stmt;
		int _inx;

		void _prepare() {
			if (sqlite3_prepare16_v2(_db, _sql.data(), -1, &_stmt, nullptr) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
		}

	protected:
		database_binder(sqlite3 * db, std::u16string const & sql) :
			_db(db),
			_sql(sql),
			_stmt(nullptr),
			_inx(1) {
			_prepare();
		}

		database_binder(sqlite3 * db, std::string const & sql) : database_binder(db, std::u16string(sql.begin(), sql.end())) { }

	public:
		friend class database;
		~database_binder() {
			/* Will be executed if no >>op is found */
			if (_stmt) {
				if (sqlite3_step(_stmt) != SQLITE_DONE) {
					throw std::runtime_error(sqlite3_errmsg(_db));
				}

				if (sqlite3_finalize(_stmt) != SQLITE_OK)
					throw std::runtime_error(sqlite3_errmsg(_db));
				_stmt = nullptr;
			}
		}

		database_binder& operator <<(double val) {
			if (sqlite3_bind_double(_stmt, _inx, val) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			++_inx;
			return *this;
		}
		database_binder& operator <<(float val) {
			if (sqlite3_bind_double(_stmt, _inx, double(val)) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			++_inx;
			return *this;
		}
		database_binder& operator <<(int val) {
			if (sqlite3_bind_int(_stmt, _inx, val) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			++_inx;
			return *this;
		}
		database_binder& operator <<(sqlite_int64 val) {
			if (sqlite3_bind_int64(_stmt, _inx, val) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			++_inx;
			return *this;
		}
		database_binder& operator <<(std::string const& txt) {
			if (sqlite3_bind_text(_stmt, _inx, txt.data(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			++_inx;
			return *this;
		}
		database_binder& operator &&(std::string const& txt) {
			if (sqlite3_bind_blob(_stmt, _inx, txt.data(), txt.size(), SQLITE_TRANSIENT) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			++_inx;
			return *this;
		}
		database_binder& operator <<(std::u16string const& txt) {
			if (sqlite3_bind_text16(_stmt, _inx, txt.data(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			++_inx;
			return *this;
		}
	};

	class database {
	private:
		sqlite3 * _db = nullptr;
		bool _connected = false;
		bool _ownes_db = false;
	public:

		database() {};
		
		void init(std::string const & db_name) {
			std::u16string name(db_name.begin(), db_name.end());
			_db = nullptr;
			_ownes_db = false;
			_connected = sqlite3_open16(name.data(), &_db) == SQLITE_OK;
		}
		
		database(std::u16string const & db_name) : _db(nullptr), _connected(false), _ownes_db(true) {
			_connected = sqlite3_open16(db_name.data(), &_db) == SQLITE_OK;
		}
		database(std::string const & db_name) : database(std::u16string(db_name.begin(), db_name.end())) { }

		database(sqlite3* db) {
			_db = db;
			_connected = SQLITE_OK;
			_ownes_db = false;
		}

		~database() {
			if (_db && _ownes_db) {
				sqlite3_close_v2(_db);
				_db = nullptr;
			}
		}

		database_binder operator<<(std::string const& sql) const {
			return database_binder(_db, sql);
		}
		database_binder operator<<(std::u16string const& sql) const {
			return database_binder(_db, sql);
		}

		operator bool() const {
			return _connected;
		}
		operator std::string() {
			return sqlite3_errmsg(_db);
		}
		operator std::u16string() {
			return (char16_t*)sqlite3_errmsg16(_db);
		}
	};

}
