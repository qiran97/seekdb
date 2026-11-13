#include "sql.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>

namespace mvdb {

int TableDef::column_index(const std::string& column_name) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (iequals(columns[i].name, column_name)) return static_cast<int>(i);
  }
  return -1;
}

std::vector<int> TableDef::value_columns() const {
  std::vector<int> result;
  for (size_t i = 0; i < columns.size(); ++i) {
    int index = static_cast<int>(i);
    if (index == pk_index || index == vector_index) continue;
    result.push_back(index);
  }
  return result;
}

namespace {

enum class TokenType { kEnd, kIdent, kNumber, kString, kPunct };

struct Token {
  TokenType type = TokenType::kEnd;
  std::string text;       // identifier / punctuation text
  std::string string_value;  // decoded string literal
  bool is_float = false;
};

class Lexer {
 public:
  explicit Lexer(const std::string& text) : text_(text) {}

  bool next(Token* token, Status* error) {
    skip_space_and_comments();
    token_start_ = pos_;
    if (pos_ >= text_.size()) {
      token->type = TokenType::kEnd;
      return true;
    }
    char c = text_[pos_];
    if (is_ident_start(c)) return lex_identifier(token);
    if (isdigit(static_cast<unsigned char>(c))) return lex_number(token);
    // A minus in front of a digit belongs to the number.  The SQL subset
    // has no arithmetic, a minus is never an operator.
    if (c == '-' && pos_ + 1 < text_.size() &&
        isdigit(static_cast<unsigned char>(text_[pos_ + 1]))) {
      return lex_number(token);
    }
    if (c == '\'' || c == '"') return lex_string(token, error);
    return lex_punct(token);
  }

  size_t position() const { return pos_; }
  // The offset of the token that was read last.
  size_t token_start() const { return token_start_; }

 private:
  bool is_ident_start(char c) const {
    return isalpha(static_cast<unsigned char>(c)) || c == '_' ||
           static_cast<unsigned char>(c) >= 0x80;
  }
  bool is_ident_char(char c) const {
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
           static_cast<unsigned char>(c) >= 0x80;
  }

  void skip_space_and_comments() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (isspace(static_cast<unsigned char>(c))) {
        ++pos_;
        continue;
      }
      if (c == '-' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '-') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
        continue;
      }
      if (c == '#') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
        continue;
      }
      if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
        pos_ += 2;
        while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
          ++pos_;
        }
        pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
        continue;
      }
      break;
    }
  }

  bool lex_identifier(Token* token) {
    size_t start = pos_;
    while (pos_ < text_.size() && is_ident_char(text_[pos_])) ++pos_;
    token->type = TokenType::kIdent;
    token->text = text_.substr(start, pos_ - start);
    return true;
  }

  bool lex_number(Token* token) {
    size_t start = pos_;
    bool is_float = false;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      is_float = true;
      ++pos_;
      while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      size_t save = pos_;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) {
        is_float = true;
        while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
      } else {
        pos_ = save;
      }
    }
    token->type = TokenType::kNumber;
    token->text = text_.substr(start, pos_ - start);
    token->is_float = is_float;
    return true;
  }

  bool lex_string(Token* token, Status* error) {
    char quote = text_[pos_];
    ++pos_;
    std::string value;
    while (true) {
      if (pos_ >= text_.size()) {
        *error = Status::Error(ErrorCode::kSyntaxError, "unterminated string literal");
        return false;
      }
      char c = text_[pos_];
      if (c == quote) {
        if (pos_ + 1 < text_.size() && text_[pos_ + 1] == quote) {
          value.push_back(quote);
          pos_ += 2;
          continue;
        }
        ++pos_;
        break;
      }
      if (c == '\\' && pos_ + 1 < text_.size()) {
        char next = text_[pos_ + 1];
        pos_ += 2;
        switch (next) {
          case 'n': value.push_back('\n'); break;
          case 't': value.push_back('\t'); break;
          case 'r': value.push_back('\r'); break;
          case '0': value.push_back('\0'); break;
          case 'b': value.push_back('\b'); break;
          case 'Z': value.push_back('\032'); break;
          default: value.push_back(next); break;
        }
        continue;
      }
      value.push_back(c);
      ++pos_;
    }
    token->type = TokenType::kString;
    token->string_value = value;
    token->text = value;
    return true;
  }

  bool lex_punct(Token* token) {
    static const char* two_char[] = {"<=", ">=", "<>", "!=", "||", "&&", nullptr};
    if (pos_ + 1 < text_.size()) {
      std::string candidate = text_.substr(pos_, 2);
      for (int i = 0; two_char[i] != nullptr; ++i) {
        if (candidate == two_char[i]) {
          token->type = TokenType::kPunct;
          token->text = candidate;
          pos_ += 2;
          return true;
        }
      }
    }
    token->type = TokenType::kPunct;
    token->text = std::string(1, text_[pos_]);
    ++pos_;
    return true;
  }

  const std::string& text_;
  size_t pos_ = 0;
  size_t token_start_ = 0;
};

class Parser {
 public:
  explicit Parser(const std::string& sql) : lexer_(sql) {
    advance();
  }

  // The offset of the token that follows the statement that was parsed.
  size_t position() const { return lexer_.token_start(); }

  Status parse(Statement* out) {
    if (token_.type == TokenType::kEnd) {
      out->type = StatementType::kEmpty;
      return Status::Ok();
    }
    if (is_keyword("CREATE")) return parse_create(out);
    if (is_keyword("DROP")) return parse_drop(out);
    if (is_keyword("INSERT")) return parse_insert(out);
    if (is_keyword("SELECT")) return parse_select(out);
    if (is_keyword("UPDATE")) return parse_update(out);
    if (is_keyword("DELETE")) return parse_delete(out);
    if (is_keyword("BEGIN") || is_keyword("START")) return parse_transaction(out);
    if (is_keyword("COMMIT")) return parse_transaction(out);
    if (is_keyword("ROLLBACK")) return parse_transaction(out);
    if (is_keyword("SET")) return parse_set(out);
    if (is_keyword("SHOW")) return parse_show(out);
    if (is_keyword("USE") || is_keyword("MODE")) return parse_session(out);
    if (is_keyword("EXIT") || is_keyword("QUIT") || is_keyword("SHUTDOWN")) {
      out->type = StatementType::kShutdown;
      advance();
      accept_punct(";");
      return Status::Ok();
    }
    return Status::Error(ErrorCode::kSyntaxError,
                         "unsupported statement: " + token_.text);
  }

 private:
  bool is_keyword(const char* keyword) const {
    return token_.type == TokenType::kIdent && iequals(token_.text, keyword);
  }

  void advance() {
    Status error = Status::Ok();
    lexer_.next(&token_, &error);
    if (!error.ok()) {
      error_ = error;
    }
    if (getenv("MINIVECTORDB_DEBUG_TOKENS")) {
      fprintf(stderr, "[tok] type=%d text='%s'\n", static_cast<int>(token_.type),
              token_.text.c_str());
    }
  }

  // The deepest nesting of a statement that the parser accepts.
  static constexpr int kMaxParseDepth = 64;
  int depth_ = 0;

  bool accept_punct(const char* punct) {
    if (token_.type == TokenType::kPunct && token_.text == punct) {
      advance();
      return true;
    }
    return false;
  }

  bool accept_keyword(const char* keyword) {
    if (is_keyword(keyword)) {
      advance();
      return true;
    }
    return false;
  }

  Status expect_punct(const char* punct) {
    if (!accept_punct(punct)) {
      return Status::Error(ErrorCode::kSyntaxError,
                           std::string("expected '") + punct + "' near '" +
                               token_.text + "'");
    }
    return Status::Ok();
  }

  Status expect_identifier(std::string* out) {
    if (token_.type != TokenType::kIdent) {
      return Status::Error(ErrorCode::kSyntaxError,
                           "expected identifier near '" + token_.text + "'");
    }
    *out = token_.text;
    advance();
    return Status::Ok();
  }

  // --- CREATE ---------------------------------------------------------
  Status parse_create(Statement* out) {
    advance();  // CREATE
    if (accept_keyword("TABLE")) {
      out->type = StatementType::kCreateTable;
      if (accept_keyword("IF")) {
        if (!accept_keyword("NOT")) {
          return Status::Error(ErrorCode::kSyntaxError, "expected NOT EXISTS");
        }
        if (!accept_keyword("EXISTS")) {
          return Status::Error(ErrorCode::kSyntaxError, "expected EXISTS");
        }
        out->if_not_exists = true;
      }
      Status status = expect_identifier(&out->table);
      if (!status.ok()) return status;
      status = expect_punct("(");
      if (!status.ok()) return status;
      while (true) {
        ColumnDef column;
        status = expect_identifier(&column.name);
        if (!status.ok()) return status;
        if (accept_keyword("BIGINT") || accept_keyword("INT") ||
            accept_keyword("INTEGER") || accept_keyword("BIGINT")) {
          column.type = ColumnType::kBigInt;
        } else if (accept_keyword("VARCHAR")) {
          column.type = ColumnType::kVarchar;
          status = expect_punct("(");
          if (!status.ok()) return status;
          if (token_.type != TokenType::kNumber) {
            return Status::Error(ErrorCode::kSyntaxError,
                                 "VARCHAR requires a length");
          }
          int64_t length = 0;
          if (!parse_int64(token_.text, &length) || length < 0) {
            return Status::Error(ErrorCode::kSyntaxError,
                                 "invalid VARCHAR length: " + token_.text);
          }
          column.length = static_cast<uint32_t>(length);
          advance();
          status = expect_punct(")");
          if (!status.ok()) return status;
        } else if (accept_keyword("VECTOR")) {
          column.type = ColumnType::kVector;
          status = expect_punct("(");
          if (!status.ok()) return status;
          if (token_.type != TokenType::kNumber) {
            return Status::Error(ErrorCode::kSyntaxError,
                                 "VECTOR requires a dimension");
          }
          int64_t dim = 0;
          if (!parse_int64(token_.text, &dim) || dim <= 0) {
            return Status::Error(ErrorCode::kSyntaxError,
                                 "invalid VECTOR dimension: " + token_.text);
          }
          column.dim = static_cast<uint32_t>(dim);
          advance();
          status = expect_punct(")");
          if (!status.ok()) return status;
        } else {
          return Status::Error(ErrorCode::kSyntaxError,
                               "unsupported column type near '" + token_.text + "'");
        }
        if (accept_keyword("PRIMARY")) {
          if (!accept_keyword("KEY")) {
            return Status::Error(ErrorCode::kSyntaxError, "expected PRIMARY KEY");
          }
          column.primary_key = true;
        }
        if (accept_keyword("NOT")) {
          if (!accept_keyword("NULL")) {
            return Status::Error(ErrorCode::kSyntaxError, "expected NOT NULL");
          }
        }
        out->table_def.columns.push_back(column);
        if (accept_punct(",")) continue;
        break;
      }
      status = expect_punct(")");
      if (!status.ok()) return status;
      accept_punct(";");
      return Status::Ok();
    }
    if (accept_keyword("VECTOR")) {
      if (!accept_keyword("INDEX")) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "expected CREATE VECTOR INDEX");
      }
      out->type = StatementType::kCreateVectorIndex;
      Status status = expect_identifier(&out->index_name);
      if (!status.ok()) return status;
      if (!accept_keyword("ON")) {
        return Status::Error(ErrorCode::kSyntaxError,
                             "expected ON in CREATE VECTOR INDEX");
      }
      status = expect_identifier(&out->table);
      if (!status.ok()) return status;
      status = expect_punct("(");
      if (!status.ok()) return status;
      status = expect_identifier(&out->index_column);
      if (!status.ok()) return status;
      status = expect_punct(")");
      if (!status.ok()) return status;
      accept_punct(";");
      return Status::Ok();
    }
    return Status::Error(ErrorCode::kSyntaxError, "unsupported CREATE statement");
  }

  // --- DROP -----------------------------------------------------------
  Status parse_drop(Statement* out) {
    advance();  // DROP
    if (accept_keyword("TABLE")) {
      out->type = StatementType::kDropTable;
      if (accept_keyword("IF")) {
        if (!accept_keyword("EXISTS")) {
          return Status::Error(ErrorCode::kSyntaxError, "expected EXISTS");
        }
        out->if_exists = true;
      }
      Status status = expect_identifier(&out->table);
      if (!status.ok()) return status;
      accept_punct(";");
      return Status::Ok();
    }
    if (accept_keyword("VECTOR")) {
      if (!accept_keyword("INDEX")) {
        return Status::Error(ErrorCode::kSyntaxError, "expected DROP VECTOR INDEX");
      }
      out->type = StatementType::kDropVectorIndex;
      Status status = expect_identifier(&out->index_name);
      if (!status.ok()) return status;
      accept_punct(";");
      return Status::Ok();
    }
    return Status::Error(ErrorCode::kSyntaxError, "unsupported DROP statement");
  }

  // --- INSERT ---------------------------------------------------------
  Status parse_insert(Statement* out) {
    advance();  // INSERT
    if (!accept_keyword("INTO")) {
      return Status::Error(ErrorCode::kSyntaxError, "expected INTO");
    }
    out->type = StatementType::kInsert;
    Status status = expect_identifier(&out->table);
    if (!status.ok()) return status;
    if (accept_punct("(")) {
      while (true) {
        std::string column;
        status = expect_identifier(&column);
        if (!status.ok()) return status;
        out->insert_columns.push_back(column);
        if (accept_punct(",")) continue;
        break;
      }
      status = expect_punct(")");
      if (!status.ok()) return status;
    }
    if (!accept_keyword("VALUES") && !accept_keyword("VALUE")) {
      return Status::Error(ErrorCode::kSyntaxError, "expected VALUES");
    }
    while (true) {
      status = expect_punct("(");
      if (!status.ok()) return status;
      std::vector<std::unique_ptr<Expr>> row;
      while (true) {
        std::unique_ptr<Expr> expr;
        status = parse_expr(&expr);
        if (!status.ok()) return status;
        row.push_back(std::move(expr));
        if (accept_punct(",")) continue;
        break;
      }
      status = expect_punct(")");
      if (!status.ok()) return status;
      out->insert_rows.push_back(std::move(row));
      if (accept_punct(",")) continue;
      break;
    }
    accept_punct(";");
    return Status::Ok();
  }

  // --- SELECT ---------------------------------------------------------
  Status parse_select(Statement* out) {
    advance();  // SELECT
    out->type = StatementType::kSelect;
    if (accept_punct("*")) {
      out->select_star = true;
    } else {
      while (true) {
        std::unique_ptr<Expr> expr;
        Status status = parse_expr(&expr);
        if (!status.ok()) return status;
        if (accept_keyword("AS")) {
          status = expect_identifier(&expr->alias);
          if (!status.ok()) return status;
        } else if (token_.type == TokenType::kIdent &&
                   !is_clause_start(token_.text)) {
          expr->alias = token_.text;
          advance();
        }
        out->select_list.push_back(std::move(expr));
        if (accept_punct(",")) continue;
        break;
      }
    }
    if (accept_keyword("FROM")) {
      Status status = expect_identifier(&out->table);
      if (!status.ok()) return status;
    }
    if (accept_keyword("WHERE")) {
      std::unique_ptr<Condition> condition;
      Status status = parse_condition(&condition);
      if (!status.ok()) return status;
      out->where = std::move(condition);
    }
    if (accept_keyword("ORDER")) {
      if (!accept_keyword("BY")) {
        return Status::Error(ErrorCode::kSyntaxError, "expected BY after ORDER");
      }
      while (true) {
        OrderByItem item;
        if (token_.type == TokenType::kNumber && !token_.is_float) {
          int64_t ordinal = 0;
          Status status = parse_number(token_.text, &ordinal);
          if (!status.ok()) return status;
          if (ordinal <= 0 || ordinal > 0x7fffffff) {
            return Status::Error(ErrorCode::kSyntaxError,
                                 "the ORDER BY position is out of range: " +
                                     token_.text);
          }
          item.ordinal = static_cast<int>(ordinal);
          advance();
        } else {
          Status status = parse_expr(&item.expr);
          if (!status.ok()) return status;
        }
        if (accept_keyword("ASC")) {
          item.ascending = true;
        } else if (accept_keyword("DESC")) {
          item.ascending = false;
        }
        out->order_by.push_back(std::move(item));
        if (accept_punct(",")) continue;
        break;
      }
    }
    if (accept_keyword("LIMIT")) {
      if (token_.type != TokenType::kNumber) {
        return Status::Error(ErrorCode::kSyntaxError, "expected LIMIT value");
      }
      int64_t first = 0;
      Status status = parse_number(token_.text, &first);
      if (!status.ok()) return status;
      advance();
      if (accept_punct(",")) {
        // MySQL style "LIMIT offset, count".
        if (token_.type != TokenType::kNumber) {
          return Status::Error(ErrorCode::kSyntaxError, "expected LIMIT value");
        }
        int64_t second = 0;
        Status status = parse_number(token_.text, &second);
        if (!status.ok()) return status;
        advance();
        out->offset = first;
        out->limit = second;
      } else {
        out->limit = first;
        if (accept_keyword("OFFSET")) {
          if (token_.type != TokenType::kNumber) {
            return Status::Error(ErrorCode::kSyntaxError, "expected OFFSET value");
          }
          int64_t offset = 0;
          Status status = parse_number(token_.text, &offset);
          if (!status.ok()) return status;
          advance();
          out->offset = offset;
        }
      }
      if (out->limit < 0 || out->offset < 0) {
        return Status::Error(ErrorCode::kSyntaxError, "LIMIT must not be negative");
      }
    }
    accept_punct(";");
    return Status::Ok();
  }

  bool is_clause_start(const std::string& word) const {
    return iequals(word, "from") || iequals(word, "where") ||
           iequals(word, "order") || iequals(word, "limit") ||
           iequals(word, "and") || iequals(word, "or") || iequals(word, "not");
  }

  // --- UPDATE / DELETE ------------------------------------------------
  Status parse_update(Statement* out) {
    advance();  // UPDATE
    out->type = StatementType::kUpdate;
    Status status = expect_identifier(&out->table);
    if (!status.ok()) return status;
    if (!accept_keyword("SET")) {
      return Status::Error(ErrorCode::kSyntaxError, "expected SET");
    }
    while (true) {
      Assignment assignment;
      status = expect_identifier(&assignment.column);
      if (!status.ok()) return status;
      status = expect_punct("=");
      if (!status.ok()) return status;
      std::unique_ptr<Expr> value;
      status = parse_expr(&value);
      if (!status.ok()) return status;
      assignment.value = std::move(value);
      out->assignments.push_back(std::move(assignment));
      if (accept_punct(",")) continue;
      break;
    }
    if (accept_keyword("WHERE")) {
      std::unique_ptr<Condition> condition;
      status = parse_condition(&condition);
      if (!status.ok()) return status;
      out->where = std::move(condition);
    }
    accept_punct(";");
    return Status::Ok();
  }

  Status parse_delete(Statement* out) {
    advance();  // DELETE
    out->type = StatementType::kDelete;
    if (!accept_keyword("FROM")) {
      return Status::Error(ErrorCode::kSyntaxError, "expected FROM");
    }
    Status status = expect_identifier(&out->table);
    if (!status.ok()) return status;
    if (accept_keyword("WHERE")) {
      std::unique_ptr<Condition> condition;
      status = parse_condition(&condition);
      if (!status.ok()) return status;
      out->where = std::move(condition);
    }
    accept_punct(";");
    return Status::Ok();
  }

  // --- transactions and session ---------------------------------------
  Status parse_transaction(Statement* out) {
    if (accept_keyword("BEGIN")) {
      accept_keyword("WORK");
      out->type = StatementType::kBegin;
      accept_punct(";");
      return Status::Ok();
    }
    if (accept_keyword("START")) {
      if (!accept_keyword("TRANSACTION")) {
        return Status::Error(ErrorCode::kSyntaxError, "expected TRANSACTION");
      }
      out->type = StatementType::kBegin;
      accept_punct(";");
      return Status::Ok();
    }
    if (accept_keyword("COMMIT")) {
      accept_keyword("WORK");
      out->type = StatementType::kCommit;
      accept_punct(";");
      return Status::Ok();
    }
    if (accept_keyword("ROLLBACK")) {
      accept_keyword("WORK");
      out->type = StatementType::kRollback;
      accept_punct(";");
      return Status::Ok();
    }
    return Status::Error(ErrorCode::kSyntaxError,
                         "unsupported transaction statement near '" + token_.text + "'");
  }

  Status parse_set(Statement* out) {
    advance();  // SET
    out->type = StatementType::kSet;
    Status status = expect_identifier(&out->name);
    if (!status.ok()) return status;
    if (!accept_punct("=")) {
      // SET NAMES 'utf8mb4' style statements are accepted and ignored.
      if (token_.type == TokenType::kString) {
        advance();
        out->name = "__ignored__";
        out->value = "";
        accept_punct(";");
        return Status::Ok();
      }
      return Status::Error(ErrorCode::kSyntaxError, "expected '=' in SET");
    }
    if (token_.type == TokenType::kString) {
      out->value = token_.string_value;
      advance();
    } else if (token_.type == TokenType::kNumber) {
      out->value = token_.text;
      advance();
    } else if (token_.type == TokenType::kIdent) {
      out->value = token_.text;
      advance();
    } else {
      return Status::Error(ErrorCode::kSyntaxError, "invalid SET value");
    }
    accept_punct(";");
    return Status::Ok();
  }

  Status parse_show(Statement* out) {
    advance();  // SHOW
    out->type = StatementType::kShowTables;
    if (!accept_keyword("TABLES")) {
      return Status::Error(ErrorCode::kSyntaxError, "unsupported SHOW statement");
    }
    accept_punct(";");
    return Status::Ok();
  }

  Status parse_session(Statement* out) {
    advance();  // USE / MODE
    out->type = StatementType::kSetSearchMode;
    Status status = expect_identifier(&out->value);
    if (!status.ok()) return status;
    accept_punct(";");
    return Status::Ok();
  }

  // --- expressions ----------------------------------------------------
  // Reads a number that has to fit into a 64 bit integer.  A number that is
  // too big is an error, it must not become a zero.
  Status parse_number(std::string_view text, int64_t* out) {
    if (!parse_int64(text, out)) {
      return Status::Error(ErrorCode::kNumberOutOfRange,
                           "the number is out of range: " + std::string(text));
    }
    return Status::Ok();
  }

  // The parser is recursive.  A statement with a deep nesting must not
  // exhaust the stack of the process, the depth is limited instead.
  class Depth {
   public:
    explicit Depth(int* depth) : depth_(depth) { ++(*depth_); }
    ~Depth() { --(*depth_); }
    bool too_deep() const { return *depth_ > kMaxParseDepth; }

   private:
    int* depth_;
  };

  Status parse_expr(std::unique_ptr<Expr>* out) {
    Depth depth(&depth_);
    if (depth.too_deep()) {
      return Status::Error(ErrorCode::kSyntaxError,
                           "the statement is nested too deeply");
    }
    if (token_.type == TokenType::kNumber) {
      std::unique_ptr<Expr> expr(new Expr());
      if (token_.is_float) {
        expr->type = ExprType::kStringLiteral;
        expr->str_value = token_.text;
      } else {
        expr->type = ExprType::kIntLiteral;
        int64_t value = 0;
        Status status = parse_number(token_.text, &value);
        if (!status.ok()) return status;
        expr->int_value = value;
      }
      advance();
      *out = std::move(expr);
      return Status::Ok();
    }
    if (token_.type == TokenType::kString) {
      std::unique_ptr<Expr> expr(new Expr());
      expr->type = ExprType::kStringLiteral;
      expr->str_value = token_.string_value;
      advance();
      *out = std::move(expr);
      return Status::Ok();
    }
    if (token_.type == TokenType::kIdent) {
      std::string name = token_.text;
      advance();
      if (token_.type == TokenType::kPunct && token_.text == "(") {
        advance();  // consume the opening parenthesis
        if (!iequals(name, "L2_DISTANCE")) {
          return Status::Error(ErrorCode::kSyntaxError,
                               "unsupported function: " + name);
        }
        std::unique_ptr<Expr> expr(new Expr());
        expr->type = ExprType::kL2Distance;
        while (true) {
          std::unique_ptr<Expr> argument;
          Status status = parse_expr(&argument);
          if (!status.ok()) return status;
          expr->children.push_back(std::move(argument));
          if (accept_punct(",")) continue;
          break;
        }
        Status status = expect_punct(")");
        if (!status.ok()) return status;
        if (expr->children.size() != 2) {
          return Status::Error(ErrorCode::kSyntaxError,
                               "L2_DISTANCE expects exactly two arguments");
        }
        *out = std::move(expr);
        return Status::Ok();
      }
      std::unique_ptr<Expr> expr(new Expr());
      expr->type = ExprType::kColumn;
      expr->name = name;
      *out = std::move(expr);
      return Status::Ok();
    }
    return Status::Error(ErrorCode::kSyntaxError,
                         "unexpected token in expression: '" + token_.text + "'");
  }

  // --- conditions -----------------------------------------------------
  Status parse_condition(std::unique_ptr<Condition>* out) {
    return parse_or(out);
  }

  Status parse_or(std::unique_ptr<Condition>* out) {
    std::unique_ptr<Condition> left;
    Status status = parse_and(&left);
    if (!status.ok()) return status;
    if (!is_keyword("OR")) {
      *out = std::move(left);
      return Status::Ok();
    }
    std::unique_ptr<Condition> node(new Condition());
    node->type = ConditionType::kOr;
    node->children.push_back(std::move(left));
    while (accept_keyword("OR")) {
      std::unique_ptr<Condition> right;
      status = parse_and(&right);
      if (!status.ok()) return status;
      node->children.push_back(std::move(right));
    }
    *out = std::move(node);
    return Status::Ok();
  }

  Status parse_and(std::unique_ptr<Condition>* out) {
    std::unique_ptr<Condition> left;
    Status status = parse_not(&left);
    if (!status.ok()) return status;
    if (!is_keyword("AND")) {
      *out = std::move(left);
      return Status::Ok();
    }
    std::unique_ptr<Condition> node(new Condition());
    node->type = ConditionType::kAnd;
    node->children.push_back(std::move(left));
    while (accept_keyword("AND")) {
      std::unique_ptr<Condition> right;
      status = parse_not(&right);
      if (!status.ok()) return status;
      node->children.push_back(std::move(right));
    }
    *out = std::move(node);
    return Status::Ok();
  }

  Status parse_not(std::unique_ptr<Condition>* out) {
    Depth depth(&depth_);
    if (depth.too_deep()) {
      return Status::Error(ErrorCode::kSyntaxError,
                           "the statement is nested too deeply");
    }
    if (accept_keyword("NOT")) {
      std::unique_ptr<Condition> child;
      Status status = parse_not(&child);
      if (!status.ok()) return status;
      std::unique_ptr<Condition> node(new Condition());
      node->type = ConditionType::kNot;
      node->children.push_back(std::move(child));
      *out = std::move(node);
      return Status::Ok();
    }
    return parse_comparison(out);
  }

  Status parse_comparison(std::unique_ptr<Condition>* out) {
    Depth depth(&depth_);
    if (depth.too_deep()) {
      return Status::Error(ErrorCode::kSyntaxError,
                           "the statement is nested too deeply");
    }
    std::unique_ptr<Condition> node(new Condition());
    node->type = ConditionType::kCompare;
    bool parenthesised = false;
    if (token_.type == TokenType::kPunct && token_.text == "(") {
      advance();
      parenthesised = true;
    }
    Status status = parse_expr(&node->left);
    if (!status.ok()) return status;
    CompareOp op;
    bool has_op = true;
    if (accept_punct("=")) {
      op = CompareOp::kEq;
    } else if (accept_punct("<=")) {
      op = CompareOp::kLe;
    } else if (accept_punct(">=")) {
      op = CompareOp::kGe;
    } else if (accept_punct("<")) {
      op = CompareOp::kLt;
    } else if (accept_punct(">")) {
      op = CompareOp::kGt;
    } else if (accept_punct("<>") || accept_punct("!=")) {
      op = CompareOp::kNe;
    } else {
      has_op = false;
    }
    if (!has_op) {
      return Status::Error(ErrorCode::kSyntaxError,
                           "expected a comparison operator near '" + token_.text + "'");
    }
    node->op = op;
    status = parse_expr(&node->right);
    if (!status.ok()) return status;
    if (parenthesised) {
      status = expect_punct(")");
      if (!status.ok()) return status;
    }
    *out = std::move(node);
    return Status::Ok();
  }

  Lexer lexer_;
  Token token_;
  Status error_ = Status::Ok();
};

}  // namespace

Status parse_statement(const std::string& sql, Statement* out) {
  Parser parser(sql);
  Status status = parser.parse(out);
  if (!status.ok()) return status;
  return Status::Ok();
}

Status parse_first_statement(const std::string& sql, Statement* out,
                              size_t* length) {
  Parser parser(sql);
  Status status = parser.parse(out);
  if (!status.ok()) return status;
  *length = parser.position();
  return Status::Ok();
}

bool only_statement_separators(const std::string& text) {
  size_t i = 0;
  const size_t size = text.size();
  while (i < size) {
    char c = text[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == ';') {
      ++i;
      continue;
    }
    if (c == '-' && i + 1 < size && text[i + 1] == '-') {
      while (i < size && text[i] != '\n') ++i;
      continue;
    }
    if (c == '#') {
      while (i < size && text[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < size && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < size && !(text[i] == '*' && text[i + 1] == '/')) ++i;
      i = (i + 1 < size) ? i + 2 : size;
      continue;
    }
    return false;
  }
  return true;
}

std::vector<std::string> split_statements(const std::string& script) {
  std::vector<std::string> statements;
  std::string current;
  size_t i = 0;
  const size_t size = script.size();
  bool in_single = false;
  bool in_double = false;
  bool in_backtick = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  while (i < size) {
    char c = script[i];
    if (in_line_comment) {
      current.push_back(c);
      if (c == '\n') in_line_comment = false;
      ++i;
      continue;
    }
    if (in_block_comment) {
      current.push_back(c);
      if (c == '*' && i + 1 < size && script[i + 1] == '/') {
        current.push_back('/');
        ++i;
        in_block_comment = false;
      }
      ++i;
      continue;
    }
    if (!in_single && !in_double && !in_backtick) {
      if (c == '-' && i + 1 < size && script[i + 1] == '-') {
        in_line_comment = true;
        current.push_back(c);
        ++i;
        continue;
      }
      if (c == '#') {
        in_line_comment = true;
        current.push_back(c);
        ++i;
        continue;
      }
      if (c == '/' && i + 1 < size && script[i + 1] == '*') {
        in_block_comment = true;
        current.push_back(c);
        ++i;
        continue;
      }
    }
    if (in_single) {
      current.push_back(c);
      if (c == '\\' && i + 1 < size) {
        current.push_back(script[i + 1]);
        i += 2;
        continue;
      }
      if (c == '\'') {
        if (i + 1 < size && script[i + 1] == '\'') {
          current.push_back('\'');
          i += 2;
          continue;
        }
        in_single = false;
      }
      ++i;
      continue;
    }
    if (in_double) {
      current.push_back(c);
      if (c == '\\' && i + 1 < size) {
        current.push_back(script[i + 1]);
        i += 2;
        continue;
      }
      if (c == '"') {
        if (i + 1 < size && script[i + 1] == '"') {
          current.push_back('"');
          i += 2;
          continue;
        }
        in_double = false;
      }
      ++i;
      continue;
    }
    if (in_backtick) {
      current.push_back(c);
      if (c == '`') in_backtick = false;
      ++i;
      continue;
    }
    if (c == '\'') {
      in_single = true;
      current.push_back(c);
      ++i;
      continue;
    }
    if (c == '"') {
      in_double = true;
      current.push_back(c);
      ++i;
      continue;
    }
    if (c == '`') {
      in_backtick = true;
      current.push_back(c);
      ++i;
      continue;
    }
    if (c == ';') {
      std::string statement = trim(current);
      if (!statement.empty()) statements.push_back(statement);
      current.clear();
      ++i;
      continue;
    }
    current.push_back(c);
    ++i;
  }
  std::string statement = trim(current);
  if (!statement.empty()) statements.push_back(statement);
  return statements;
}

}  // namespace mvdb