#pragma once
#include<string>
#include<functional>
#include<stdexcept>
#include<iostream>
#include"sqlite3.h"

/*
	This is an earlier version of sqlite_modern_cpp (current versions break on OS X), lightly patched
	to include a database.init() method, and to use the && operator for inserting blobs.
	https://github.com/SqliteModernCpp/sqlite_modern_cpp/commit/552541a0afc49d9381aa75cb9ae5a9c362b0fdea
	also applied.
	-- Richard Fairhurst, 28.06.2015
*/

namespace sqlite {

#pragma region function_traits
	template <typename T>
	struct function_traits : public function_traits < decltype(&T::operator()) > {};

	template <typename ClassType, typename ReturnType, typename... Args>
	struct function_traits < ReturnType(ClassType::*)(Args...) const >
		// we specialize for pointers to member function
	{
		enum { arity = sizeof...(Args) };
		// arity is the number of arguments.

		typedef ReturnType result_type;

		template <size_t i>
		struct arg {
			typedef typename std::tuple_element<i, std::tuple<Args...>>::type type;
			// the i-th argument is equivalent to the i-th tuple element of a tuple
			// composed of those arguments.
		};
	};

#pragma endregion

	class database;
	class database_binder;

	template<int N>
	class binder {
		template<typename F>
		static void run(database_binder& dbb, F l);
	};

	class database_binder {
	private:
		sqlite3 * _db;
		std::string _sql;
		sqlite3_stmt* _stmt;
		int _inx;

		void _extract(std::function<void(void)> call_back) {
			int hresult;
			while ((hresult = sqlite3_step(_stmt)) == SQLITE_ROW)
				call_back();

			if (hresult != SQLITE_DONE)
				throw std::runtime_error(sqlite3_errmsg(_db));

			if (sqlite3_finalize(_stmt) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			_stmt = nullptr;
		}
		void _extract_single_value(std::function<void(void)> call_back) {
			int hresult;
			if ((hresult = sqlite3_step(_stmt)) == SQLITE_ROW)
				call_back();

			if ((hresult = sqlite3_step(_stmt)) == SQLITE_ROW)
				throw std::runtime_error("not every row extracted");

			if (hresult != SQLITE_DONE)
				throw std::runtime_error(sqlite3_errmsg(_db));

			if (sqlite3_finalize(_stmt) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
			_stmt = nullptr;
		}
		void _prepare() {
			if (sqlite3_prepare_v2(_db, _sql.data(), -1, &_stmt, nullptr) != SQLITE_OK)
				throw std::runtime_error(sqlite3_errmsg(_db));
		}
	protected:
		database_binder(sqlite3 * db, std::string const & sql) :
			_db(db),
			_sql(sql),
			_stmt(nullptr),
			_inx(1) {
			_prepare();
		}

	public:
		friend class database;
		~database_binder() noexcept(false) {
			/* Will be executed if no >>op is found */
			if (_stmt) {
				int hresult;
				while ((hresult=sqlite3_step(_stmt)) == SQLITE_ROW) {}
				
				if (hresult != SQLITE_DONE) {
					throw std::runtime_error(sqlite3_errmsg(_db));
				}

				if (sqlite3_finalize(_stmt) != SQLITE_OK)
					throw std::runtime_error(sqlite3_errmsg(_db));
				_stmt = nullptr;
			}
		}
#pragma region operator <<
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
#pragma endregion

#pragma region get_col_from_db
		void get_col_from_db(int inx, int& i) {
			if (sqlite3_column_type(_stmt, inx) == SQLITE_NULL) i = 0;
			else i = sqlite3_column_int(_stmt, inx);
		}
		void get_col_from_db(int inx, sqlite3_int64& i) {
			if (sqlite3_column_type(_stmt, inx) == SQLITE_NULL) i = 0;
			else i = sqlite3_column_int64(_stmt, inx);
		}
		void get_col_from_db(int inx, std::string& s) {
			if (sqlite3_column_type(_stmt, inx) == SQLITE_NULL) s = std::string();
			else {
				sqlite3_column_bytes(_stmt, inx);
				s = std::string((char*)sqlite3_column_text(_stmt, inx));
			}
		}
		void get_col_from_db(int inx, std::vector<char>& v) {
			if (sqlite3_column_type(_stmt, inx) == SQLITE_NULL) v = std::vector<char>();
			else {
				int size = sqlite3_column_bytes(_stmt, inx);
				char* p = (char*)sqlite3_column_blob(_stmt,inx);
				v = std::vector<char>(p, p+size);
			}
		}
		void get_col_from_db(int inx, double& d) {
			if (sqlite3_column_type(_stmt, inx) == SQLITE_NULL) d = 0;
			else d = sqlite3_column_double(_stmt, inx);
		}
		void get_col_from_db(int inx, float& f) {
			if (sqlite3_column_type(_stmt, inx) == SQLITE_NULL) f = 0;
			else f = float(sqlite3_column_double(_stmt, inx));
		}
#pragma endregion

#pragma region operator >>

		void operator>>(int & val) {
			_extract_single_value([&] {
				get_col_from_db(0, val);
			});
		}
		void operator>>(std::string& val) {
			_extract_single_value([&] {
				get_col_from_db(0, val);
			});
		}
		void operator>>(std::vector<char>& val) {
			_extract_single_value([&] {
				get_col_from_db(0, val);
			});
		}
		void operator>>(double & val) {
			_extract_single_value([&] {
				get_col_from_db(0, val);
			});
		}
		void operator>>(float & val) {
			_extract_single_value([&] {
				get_col_from_db(0, val);
			});
		}
		void operator>>(sqlite3_int64 & val) {
			_extract_single_value([&] {
				get_col_from_db(0, val);
			});
		}

		template<typename FUNC>
		void operator>>(FUNC l) {
			typedef function_traits<decltype(l)> traits;

			database_binder& dbb = *this;
			_extract([&]() {
				binder<traits::arity>::run(dbb, l);
			});

		}
#pragma endregion

	};

	class database {
	private:
		sqlite3 * _db = nullptr;
		bool _connected = false;
		bool _ownes_db = false;
	public:

		database() {};
		
		void init(std::string const & db_name) {
			_db = nullptr;
			_ownes_db = false;
			_connected = sqlite3_open(db_name.data(), &_db) == SQLITE_OK;
		}
		
		database(std::string const & db_name) : _db(nullptr), _connected(false), _ownes_db(true) {
			_connected = sqlite3_open(db_name.data(), &_db) == SQLITE_OK;
		}

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

		operator bool() const {
			return _connected;
		}
		operator std::string() {
			return sqlite3_errmsg(_db);
		}
	};

#pragma region binder specialization

	template<>
	struct binder < 0 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;

			l();
		}
	};
	template<>
	struct binder < 1 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits::template arg<0>::type type_1;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);

			l(std::move(col_1));
		}
	};
	template<>
	struct binder < 2 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits::template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;

			type_1 col_1;
			type_2 col_2;
			dbb.get_col_from_db(0, col_1);
			dbb.get_col_from_db(1, col_2);

			l(std::move(col_1), std::move(col_2));
		}
	};
	template<>
	struct binder < 3 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits::template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);

			l(std::move(col_1), std::move(col_2), std::move(col_3));
		}
	};
	template<>
	struct binder < 4 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits:: template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;
			typedef typename traits::template arg<3>::type type_4;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);
			type_4 col_4;
			dbb.get_col_from_db(3, col_4);

			l(std::move(col_1), std::move(col_2), std::move(col_3), std::move(col_4));
		}
	};
	template<>
	struct binder < 5 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits:: template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;
			typedef typename traits::template arg<3>::type type_4;
			typedef typename traits::template arg<4>::type type_5;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);
			type_4 col_4;
			dbb.get_col_from_db(3, col_4);
			type_5 col_5;
			dbb.get_col_from_db(4, col_5);

			l(std::move(col_1), std::move(col_2), std::move(col_3), std::move(col_4), std::move(col_5));
		}
	};
	template<>
	struct binder < 6 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits:: template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;
			typedef typename traits::template arg<3>::type type_4;
			typedef typename traits::template arg<4>::type type_5;
			typedef typename traits::template arg<5>::type type_6;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);
			type_4 col_4;
			dbb.get_col_from_db(3, col_4);
			type_5 col_5;
			dbb.get_col_from_db(4, col_5);
			type_6 col_6;
			dbb.get_col_from_db(5, col_6);

			l(std::move(col_1), std::move(col_2), std::move(col_3), std::move(col_4), std::move(col_5), std::move(col_6));
		}
	};
	template<>
	struct binder < 7 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits:: template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;
			typedef typename traits::template arg<3>::type type_4;
			typedef typename traits::template arg<4>::type type_5;
			typedef typename traits::template arg<5>::type type_6;
			typedef typename traits::template arg<6>::type type_7;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);
			type_4 col_4;
			dbb.get_col_from_db(3, col_4);
			type_5 col_5;
			dbb.get_col_from_db(4, col_5);
			type_6 col_6;
			dbb.get_col_from_db(5, col_6);
			type_7 col_7;
			dbb.get_col_from_db(6, col_7);

			l(std::move(col_1), std::move(col_2), std::move(col_3), std::move(col_4), std::move(col_5), std::move(col_6), std::move(col_7));
		}
	};
	template<>
	struct binder < 8 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits:: template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;
			typedef typename traits::template arg<3>::type type_4;
			typedef typename traits::template arg<4>::type type_5;
			typedef typename traits::template arg<5>::type type_6;
			typedef typename traits::template arg<6>::type type_7;
			typedef typename traits::template arg<7>::type type_8;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);
			type_4 col_4;
			dbb.get_col_from_db(3, col_4);
			type_5 col_5;
			dbb.get_col_from_db(4, col_5);
			type_6 col_6;
			dbb.get_col_from_db(5, col_6);
			type_7 col_7;
			dbb.get_col_from_db(6, col_7);
			type_8 col_8;
			dbb.get_col_from_db(7, col_8);

			l(std::move(col_1), std::move(col_2), std::move(col_3), std::move(col_4), std::move(col_5), std::move(col_6), std::move(col_7), std::move(col_8));
		}
	};
	template<>
	struct binder < 9 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits:: template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;
			typedef typename traits::template arg<3>::type type_4;
			typedef typename traits::template arg<4>::type type_5;
			typedef typename traits::template arg<5>::type type_6;
			typedef typename traits::template arg<6>::type type_7;
			typedef typename traits::template arg<7>::type type_8;
			typedef typename traits::template arg<8>::type type_9;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);
			type_4 col_4;
			dbb.get_col_from_db(3, col_4);
			type_5 col_5;
			dbb.get_col_from_db(4, col_5);
			type_6 col_6;
			dbb.get_col_from_db(5, col_6);
			type_7 col_7;
			dbb.get_col_from_db(6, col_7);
			type_8 col_8;
			dbb.get_col_from_db(7, col_8);
			type_9 col_9;
			dbb.get_col_from_db(8, col_9);

			l(std::move(col_1), std::move(col_2), std::move(col_3), std::move(col_4), std::move(col_5), std::move(col_6), std::move(col_7), std::move(col_8), std::move(col_9));
		}
	};
	template<>
	struct binder < 10 > {
		template<typename F>
		static void run(database_binder& dbb, F l) {
			typedef function_traits<decltype(l)> traits;
			typedef typename traits:: template arg<0>::type type_1;
			typedef typename traits::template arg<1>::type type_2;
			typedef typename traits::template arg<2>::type type_3;
			typedef typename traits::template arg<3>::type type_4;
			typedef typename traits::template arg<4>::type type_5;
			typedef typename traits::template arg<5>::type type_6;
			typedef typename traits::template arg<6>::type type_7;
			typedef typename traits::template arg<7>::type type_8;
			typedef typename traits::template arg<8>::type type_9;
			typedef typename traits::template arg<9>::type type_10;

			type_1 col_1;
			dbb.get_col_from_db(0, col_1);
			type_2 col_2;
			dbb.get_col_from_db(1, col_2);
			type_3 col_3;
			dbb.get_col_from_db(2, col_3);
			type_4 col_4;
			dbb.get_col_from_db(3, col_4);
			type_5 col_5;
			dbb.get_col_from_db(4, col_5);
			type_6 col_6;
			dbb.get_col_from_db(5, col_6);
			type_7 col_7;
			dbb.get_col_from_db(6, col_7);
			type_8 col_8;
			dbb.get_col_from_db(7, col_8);
			type_9 col_9;
			dbb.get_col_from_db(8, col_9);
			type_9 col_10;
			dbb.get_col_from_db(9, col_10);

			l(std::move(col_1), std::move(col_2), std::move(col_3), std::move(col_4), std::move(col_5), std::move(col_6), std::move(col_7), std::move(col_8), std::move(col_9), std::move(col_10));
		}
	};
#pragma endregion

}
