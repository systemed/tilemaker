#pragma once

#include <string>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <ctime>
#include <tuple>
#include <memory>
#include <vector>

#ifdef _MODERN_SQLITE_BOOST_OPTIONAL_SUPPORT
#include <boost/optional.hpp>
#endif

#include <sqlite3.h>

#include <sqlite_modern_cpp/utility/function_traits.h>

namespace sqlite {

	struct sqlite_exception: public std::runtime_error {
		sqlite_exception(const char* msg):runtime_error(msg) {}
	};

	namespace exceptions {
		//One more or less trivial derived error class for each SQLITE error.
		//Note the following are not errors so have no classes:
		//SQLITE_OK, SQLITE_NOTICE, SQLITE_WARNING, SQLITE_ROW, SQLITE_DONE
		//
		//Note these names are exact matches to the names of the SQLITE error codes.
		class error: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class internal: public sqlite_exception{ using sqlite_exception::sqlite_exception; };
		class perm: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class abort: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class busy: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class locked: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class nomem: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class readonly: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class interrupt: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class ioerr: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class corrupt: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class notfound: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class full: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class cantopen: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class protocol: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class empty: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class schema: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class toobig: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class constraint: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class mismatch: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class misuse: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class nolfs: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class auth: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class format: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class range: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class notadb: public sqlite_exception { using sqlite_exception::sqlite_exception; };

		//Some additional errors are here for the C++ interface
		class more_rows: public sqlite_exception { using sqlite_exception::sqlite_exception; };
		class no_rows: public sqlite_exception { using sqlite_exception::sqlite_exception; };

		static void throw_sqlite_error(const int& error_code) {
			if(error_code == SQLITE_ERROR) throw exceptions::error(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_INTERNAL) throw exceptions::internal  (sqlite3_errstr(error_code));
			else if(error_code == SQLITE_PERM) throw exceptions::perm(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_ABORT) throw exceptions::abort(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_BUSY) throw exceptions::busy(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_LOCKED) throw exceptions::locked(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_NOMEM) throw exceptions::nomem(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_READONLY) throw exceptions::readonly(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_INTERRUPT) throw exceptions::interrupt(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_IOERR) throw exceptions::ioerr(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_CORRUPT) throw exceptions::corrupt(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_NOTFOUND) throw exceptions::notfound(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_FULL) throw exceptions::full(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_CANTOPEN) throw exceptions::cantopen(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_PROTOCOL) throw exceptions::protocol(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_EMPTY) throw exceptions::empty(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_SCHEMA) throw exceptions::schema(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_TOOBIG) throw exceptions::toobig(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_CONSTRAINT) throw exceptions::constraint(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_MISMATCH) throw exceptions::mismatch(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_MISUSE) throw exceptions::misuse(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_NOLFS) throw exceptions::nolfs(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_AUTH) throw exceptions::auth(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_FORMAT) throw exceptions::format(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_RANGE) throw exceptions::range(sqlite3_errstr(error_code));
			else if(error_code == SQLITE_NOTADB) throw exceptions::notadb(sqlite3_errstr(error_code));
			else throw sqlite_exception(sqlite3_errstr(error_code));
		}
	}

	class database;
	class database_binder;

	template<std::size_t> class binder;

	typedef std::shared_ptr<sqlite3> connection_type;

	template<typename Tuple, int Element = 0, bool Last = (std::tuple_size<Tuple>::value == Element)> struct tuple_iterate {
		static void iterate(Tuple& t, database_binder& db) {
			get_col_from_db(db, Element, std::get<Element>(t));
			tuple_iterate<Tuple, Element + 1>::iterate(t, db);
		}
	};

	template<typename Tuple, int Element> struct tuple_iterate<Tuple, Element, true> {
		static void iterate(Tuple&, database_binder&) {}
	};

	class database_binder {

	public:
		// database_binder is not copyable
		database_binder() = delete;
		database_binder(const database_binder& other) = delete;
		database_binder& operator=(const database_binder&) = delete;

		database_binder(database_binder&& other) :
			_db(std::move(other._db)),
			_sql(std::move(other._sql)),
			_stmt(std::move(other._stmt)),
			_inx(other._inx), execution_started(other.execution_started) { }

		void reset() {
			sqlite3_reset(_stmt.get());
			sqlite3_clear_bindings(_stmt.get());
			_inx = 1;
			used(false);
		}

		void execute() {
			int hresult;

			while((hresult = sqlite3_step(_stmt.get())) == SQLITE_ROW) {}

			if(hresult != SQLITE_DONE) {
				exceptions::throw_sqlite_error(hresult);
			}
			used(true); /* prevent from executing again when goes out of scope */
		}

		void used(bool state) { execution_started = state; }
		bool used() const { return execution_started; }

	private:
		std::shared_ptr<sqlite3> _db;
		std::u16string _sql;
		std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> _stmt;

		int _inx;

		bool execution_started = false;

		void _extract(std::function<void(void)> call_back) {
			execution_started = true;
			int hresult;

			while((hresult = sqlite3_step(_stmt.get())) == SQLITE_ROW) {
				call_back();
			}

			if(hresult != SQLITE_DONE) {
				exceptions::throw_sqlite_error(hresult);
			}
			reset();
		}

		void _extract_single_value(std::function<void(void)> call_back) {
			execution_started = true;
			int hresult;

			if((hresult = sqlite3_step(_stmt.get())) == SQLITE_ROW) {
				call_back();
			} else if(hresult == SQLITE_DONE) {
				throw exceptions::no_rows("no rows to extract: exactly 1 row expected");
			}

			if((hresult = sqlite3_step(_stmt.get())) == SQLITE_ROW) {
				throw exceptions::more_rows("not all rows extracted");
			}

			if(hresult != SQLITE_DONE) {
				exceptions::throw_sqlite_error(hresult);
			}
			reset();
		}

		sqlite3_stmt* _prepare(const std::u16string& sql) {
			int hresult;
			sqlite3_stmt* tmp = nullptr;
			hresult = sqlite3_prepare16_v2(_db.get(), sql.data(), -1, &tmp, nullptr);
			if((hresult) != SQLITE_OK) exceptions::throw_sqlite_error(hresult);
			return tmp;
		}

		template <typename Type>
		struct is_sqlite_value : public std::integral_constant<
			bool,
			std::is_floating_point<Type>::value
			|| std::is_integral<Type>::value
			|| std::is_same<std::string, Type>::value
			|| std::is_same<std::u16string, Type>::value
			|| std::is_same<sqlite_int64, Type>::value
		> { };
		template <typename Type>
		struct is_sqlite_value< std::vector<Type> > : public std::integral_constant<
			bool,
			std::is_floating_point<Type>::value
			|| std::is_integral<Type>::value
			|| std::is_same<sqlite_int64, Type>::value
    > { };


		template<typename T> friend database_binder& operator <<(database_binder& db, const T& val);
		template<typename T> friend void get_col_from_db(database_binder& db, int inx, T& val);
		/* for vector<T> support */
		template<typename T> friend database_binder& operator <<(database_binder& db, const std::vector<T>& val);
		template<typename T> friend void get_col_from_db(database_binder& db, int inx, std::vector<T>& val);
		/* for nullptr & unique_ptr support */
		friend database_binder& operator <<(database_binder& db, std::nullptr_t);
		template<typename T> friend database_binder& operator <<(database_binder& db, const std::unique_ptr<T>& val);
		template<typename T> friend void get_col_from_db(database_binder& db, int inx, std::unique_ptr<T>& val);
		template<typename T> friend T operator++(database_binder& db, int);
		// Overload instead of specializing function templates (http://www.gotw.ca/publications/mill17.htm)
		friend database_binder& operator<<(database_binder& db, const int& val);
		friend database_binder& operator&&(database_binder& db, const std::string& txt);
		friend void get_col_from_db(database_binder& db, int inx, int& val);
		friend database_binder& operator <<(database_binder& db, const sqlite_int64&  val);
		friend void get_col_from_db(database_binder& db, int inx, sqlite3_int64& i);
		friend database_binder& operator <<(database_binder& db, const float& val);
		friend void get_col_from_db(database_binder& db, int inx, float& f);
		friend database_binder& operator <<(database_binder& db, const double& val);
		friend void get_col_from_db(database_binder& db, int inx, double& d);
		friend void get_col_from_db(database_binder& db, int inx, std::string & s);
		friend database_binder& operator <<(database_binder& db, const std::string& txt);
		friend void get_col_from_db(database_binder& db, int inx, std::u16string & w);
		friend database_binder& operator <<(database_binder& db, const std::u16string& txt);


#ifdef _MODERN_SQLITE_BOOST_OPTIONAL_SUPPORT
		template <typename BoostOptionalT> friend database_binder& operator <<(database_binder& db, const boost::optional<BoostOptionalT>& val);
		template <typename BoostOptionalT> friend void get_col_from_db(database_binder& db, int inx, boost::optional<BoostOptionalT>& o);
#endif

	public:

		database_binder(std::shared_ptr<sqlite3> db, std::u16string const & sql):
			_db(db),
			_sql(sql),
			_stmt(_prepare(sql), sqlite3_finalize),
			_inx(1) {
		}

		database_binder(std::shared_ptr<sqlite3> db, std::string const & sql):
			database_binder(db, std::u16string(sql.begin(), sql.end())) {}

		~database_binder() noexcept(false) {
			/* Will be executed if no >>op is found, but not if an exception
			is in mid flight */
			if(!execution_started && !std::uncaught_exception() && _stmt) {
				execute();
			}
		}

		template <typename Result>
		typename std::enable_if<is_sqlite_value<Result>::value, void>::type operator>>(
			Result& value) {
			this->_extract_single_value([&value, this] {
				get_col_from_db(*this, 0, value);
			});
		}

		template<typename... Types>
		void operator>>(std::tuple<Types...>&& values) {
			this->_extract_single_value([&values, this] {
				tuple_iterate<std::tuple<Types...>>::iterate(values, *this);
			});
		}

		template <typename Function>
		typename std::enable_if<!is_sqlite_value<Function>::value, void>::type operator>>(
			Function&& func) {
			typedef utility::function_traits<Function> traits;

			this->_extract([&func, this]() {
				binder<traits::arity>::run(*this, func);
			});
		}
	};

	class database {
	private:
		std::shared_ptr<sqlite3> _db;
		bool _connected = false;

	public:
		database() {};

		void init(std::string const & db_name) {
			std::u16string n = std::u16string(db_name.begin(), db_name.end());
			sqlite3* tmp = nullptr;
			auto ret = sqlite3_open16(n.data(), &tmp);
			_db = std::shared_ptr<sqlite3>(tmp, [=](sqlite3* ptr) { sqlite3_close_v2(ptr); }); // this will close the connection eventually when no longer needed.
			if(ret != SQLITE_OK) exceptions::throw_sqlite_error(ret);
			_connected = true;
		}

		database(std::u16string const & db_name): _db(nullptr) {
			sqlite3* tmp = nullptr;
			auto ret = sqlite3_open16(db_name.data(), &tmp);
			_db = std::shared_ptr<sqlite3>(tmp, [=](sqlite3* ptr) { sqlite3_close_v2(ptr); }); // this will close the connection eventually when no longer needed.
			if(ret != SQLITE_OK) exceptions::throw_sqlite_error(ret);
			_connected = true;
			//_db.reset(tmp, sqlite3_close); // alternative close. (faster?)
		}

		database(std::string const & db_name):
			database(std::u16string(db_name.begin(), db_name.end())) {}

		database(std::shared_ptr<sqlite3> db):
			_db(db) {}

		database_binder operator<<(const std::string& sql) {
			return database_binder(_db, sql);
		}

		database_binder operator<<(const char* sql) {
			return *this << std::string(sql);
		}

		database_binder operator<<(const std::u16string& sql) {
			return database_binder(_db, sql);
		}

		operator bool() const {
			return _connected;
		}

		database_binder operator<<(const char16_t* sql) {
			return *this << std::u16string(sql);
		}

		connection_type connection() const { return _db; }

		sqlite3_int64 last_insert_rowid() const {
			return sqlite3_last_insert_rowid(_db.get());
		}

	};

	template<std::size_t Count>
	class binder {
	private:
		template <
			typename    Function,
			std::size_t Index
		>
			using nth_argument_type = typename utility::function_traits<
			Function
			>::template argument<Index>;

	public:
		// `Boundary` needs to be defaulted to `Count` so that the `run` function
		// template is not implicitly instantiated on class template instantiation.
		// Look up section 14.7.1 _Implicit instantiation_ of the ISO C++14 Standard
		// and the [dicussion](https://github.com/aminroosta/sqlite_modern_cpp/issues/8)
		// on Github.

		template<
			typename    Function,
			typename... Values,
			std::size_t Boundary = Count
		>
			static typename std::enable_if<(sizeof...(Values) < Boundary), void>::type run(
				database_binder& db,
				Function&&       function,
				Values&&...      values
				) {
			nth_argument_type<Function, sizeof...(Values)> value{};
			get_col_from_db(db, sizeof...(Values), value);

			run<Function>(db, function, std::forward<Values>(values)..., std::move(value));
		}

		template<
			typename    Function,
			typename... Values,
			std::size_t Boundary = Count
		>
			static typename std::enable_if<(sizeof...(Values) == Boundary), void>::type run(
				database_binder&,
				Function&&       function,
				Values&&...      values
				) {
			function(std::move(values)...);
		}
	};

	// int
	 inline database_binder& operator<<(database_binder& db, const int& val) {
		int hresult;
		if((hresult = sqlite3_bind_int(db._stmt.get(), db._inx, val)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}
		++db._inx;
		return db;
	}
	 inline void get_col_from_db(database_binder& db, int inx, int& val) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			val = 0;
		} else {
			val = sqlite3_column_int(db._stmt.get(), inx);
		}
	}

	// sqlite_int64
	 inline database_binder& operator <<(database_binder& db, const sqlite_int64&  val) {
		int hresult;
		if((hresult = sqlite3_bind_int64(db._stmt.get(), db._inx, val)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}

		++db._inx;
		return db;
	}
	 inline void get_col_from_db(database_binder& db, int inx, sqlite3_int64& i) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			i = 0;
		} else {
			i = sqlite3_column_int64(db._stmt.get(), inx);
		}
	}

	// float
	 inline database_binder& operator <<(database_binder& db, const float& val) {
		int hresult;
		if((hresult = sqlite3_bind_double(db._stmt.get(), db._inx, double(val))) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}

		++db._inx;
		return db;
	}
	 inline void get_col_from_db(database_binder& db, int inx, float& f) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			f = 0;
		} else {
			f = float(sqlite3_column_double(db._stmt.get(), inx));
		}
	}

	// double
	 inline database_binder& operator <<(database_binder& db, const double& val) {
		int hresult;
		if((hresult = sqlite3_bind_double(db._stmt.get(), db._inx, val)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}

		++db._inx;
		return db;
	}
	 inline void get_col_from_db(database_binder& db, int inx, double& d) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			d = 0;
		} else {
			d = sqlite3_column_double(db._stmt.get(), inx);
		}
	}

	// vector<T>
	template<typename T> inline database_binder& operator<<(database_binder& db, const std::vector<T>& vec) {
		void const* buf = reinterpret_cast<void const *>(vec.data());
		int bytes = vec.size() * sizeof(T);
		int hresult;
		if((hresult = sqlite3_bind_blob(db._stmt.get(), db._inx, buf, bytes, SQLITE_TRANSIENT)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}
		++db._inx;
		return db;
	}
	template<typename T> inline void get_col_from_db(database_binder& db, int inx, std::vector<T>& vec) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			vec.clear();
		} else {
			int bytes = sqlite3_column_bytes(db._stmt.get(), inx);
			T const* buf = reinterpret_cast<T const *>(sqlite3_column_blob(db._stmt.get(), inx));
			vec = std::vector<T>(buf, buf + bytes/sizeof(T));
		}
	}

	/* for nullptr support */
	inline database_binder& operator <<(database_binder& db, std::nullptr_t) {
		int hresult;
		if((hresult = sqlite3_bind_null(db._stmt.get(), db._inx)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}
		++db._inx;
		return db;
	}
	/* for nullptr support */
	template<typename T> inline database_binder& operator <<(database_binder& db, const std::unique_ptr<T>& val) {
		if(val)
			db << *val;
		else
		  db << nullptr;
		return db;
	}

	/* for unique_ptr<T> support */
	template<typename T> inline void get_col_from_db(database_binder& db, int inx, std::unique_ptr<T>& _ptr_) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			_ptr_ = nullptr;
		} else {
			auto underling_ptr = new T();
			get_col_from_db(db, inx, *underling_ptr);
			_ptr_.reset(underling_ptr);
		}
	}

	// std::string
	 inline void get_col_from_db(database_binder& db, int inx, std::string & s) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			s = std::string();
		} else {
			sqlite3_column_bytes(db._stmt.get(), inx);
			s = std::string(reinterpret_cast<char const *>(sqlite3_column_text(db._stmt.get(), inx)));
		}
	}

	// Convert char* to string to trigger op<<(..., const std::string )
	template<std::size_t N> inline database_binder& operator <<(database_binder& db, const char(&STR)[N]) { return db << std::string(STR); }
	template<std::size_t N> inline database_binder& operator <<(database_binder& db, const char16_t(&STR)[N]) { return db << std::u16string(STR); }

	 inline database_binder& operator <<(database_binder& db, const std::string& txt) {
		int hresult;
		if((hresult = sqlite3_bind_text(db._stmt.get(), db._inx, txt.data(), -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}

		++db._inx;
		return db;
	}
	inline database_binder& operator &&(database_binder& db, const std::string& txt) {
		int hresult;
		if((hresult = sqlite3_bind_blob(db._stmt.get(), db._inx, txt.data(), txt.size(), SQLITE_TRANSIENT)) != SQLITE_OK) {
			std::cout << "Error " << hresult << std::endl;
			exceptions::throw_sqlite_error(hresult);
		}

		++db._inx;
		return db;
	}
	// std::u16string
	 inline void get_col_from_db(database_binder& db, int inx, std::u16string & w) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			w = std::u16string();
		} else {
			sqlite3_column_bytes16(db._stmt.get(), inx);
			w = std::u16string(reinterpret_cast<char16_t const *>(sqlite3_column_text16(db._stmt.get(), inx)));
		}
	}


	 inline database_binder& operator <<(database_binder& db, const std::u16string& txt) {
		int hresult;
		if((hresult = sqlite3_bind_text16(db._stmt.get(), db._inx, txt.data(), -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}

		++db._inx;
		return db;
	}
	// boost::optinal support for NULL values
#ifdef _MODERN_SQLITE_BOOST_OPTIONAL_SUPPORT
	template <typename BoostOptionalT> inline database_binder& operator <<(database_binder& db, const boost::optional<BoostOptionalT>& val) {
		if(val) {
			return operator << (std::move(db), std::move(*val));
		}
		int hresult;
		if((hresult = sqlite3_bind_null(db._stmt.get(), db._inx)) != SQLITE_OK) {
			exceptions::throw_sqlite_error(hresult);
		}

		++db._inx;
		return db;
	}

	template <typename BoostOptionalT> inline void get_col_from_db(database_binder& db, int inx, boost::optional<BoostOptionalT>& o) {
		if(sqlite3_column_type(db._stmt.get(), inx) == SQLITE_NULL) {
			o.reset();
		} else {
			BoostOptionalT v;
			get_col_from_db(db, inx, v);
			o = std::move(v);
		}
	}
#endif

	// Some ppl are lazy so we have a operator for proper prep. statemant handling.
	void inline operator++(database_binder& db, int) { db.execute(); db.reset(); }

	// Convert the rValue binder to a reference and call first op<<, its needed for the call that creates the binder (be carfull of recursion here!)
	template<typename T> database_binder& operator << (database_binder&& db, const T& val) { return db << val; }

}