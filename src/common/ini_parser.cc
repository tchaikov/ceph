// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
// #define BOOST_SPIRIT_DEBUG

#include <iostream>
#include <fstream>
#include <string>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix.hpp>

namespace qi = boost::spirit::qi;
namespace ascii = boost::spirit::ascii;
namespace phoenix = boost::phoenix;

using std::cout;
using std::endl;

struct conf_item_t
{
  conf_item_t() = default;
  conf_item_t(const std::string& key, const std::string& val)
    : key{key},
      val{val}
  {}
  std::string key;
  std::string val;
};

std::ostream& operator<<(std::ostream& os, const conf_item_t item)
{
  return os << item.key << " : " << item.val << endl;
}

struct conf_section_t
{
  conf_section_t() = default;
  conf_section_t(const std::string& heading,
		 const std::vector<conf_item_t>& items)
    : heading{heading},
      items{items}
  {}
  std::string heading;
  std::vector<conf_item_t> items;
};

std::ostream& operator<<(std::ostream& os, const conf_section_t section)
{
  os << section.heading << endl;
  os << "==============" << endl;
  for (auto& item : section.items) {
    os << ">>>" << item.key << " : " << item.val << "<<<" << endl;
  }
  return os;
}

struct conf_file_t
{
  conf_file_t() = default;
  conf_file_t(const std::vector<conf_section_t>& sections)
    : sections{sections}
  {}
  std::vector<conf_section_t> sections;
};

std::ostream& operator<<(std::ostream& os, const conf_file_t& conf) {
  for (auto& section : conf.sections) {
    os << section << endl;
  }
  return os;
}

template<typename Iterator>
struct IniGrammer : qi::grammar<Iterator, conf_file_t()>
{
  IniGrammer()
    : IniGrammer::base_type(conf_file)
  {
    using qi::_1;
    using qi::_2;
    using qi::_val;
    using qi::char_;
    using qi::eoi;
    using qi::eol;
    using qi::space;
    using qi::lexeme;
    using qi::lit;
    using qi::raw;
    spaces = *space;
    comment_start %= lit('#') | lit(';');
    text_char %=
      (lit('\\') > char_) |
      (char_ - (comment_start | eol));

    key %= raw[+(text_char - (lit('=') | ' ')) % ' '];
    quoted_value %=
      lexeme[lit('"') >> *(text_char - '"') >> '"'] |
      lexeme[lit('\'') >> *(text_char - '\'') >> '\''];
    unquoted_value %= lexeme[*text_char];
    comment = spaces >> comment_start >> *(char_ - eol);
    value %= (quoted_value | unquoted_value) >> -comment;
    key_val =
      (spaces >> key >> spaces >> '=' >> spaces >> value)
      [_val = phoenix::construct<conf_item_t>(_1, _2)];

    heading %= lit('[') >> +(text_char - ']') >> ']';
    section =
      (heading >> +eol >> ((key_val - heading) % +eol))
      [_val = phoenix::construct<conf_section_t>(_1, _2)];
    conf_file =
      *(-comment >> eol) >>
      (section % +(-comment >> eol))[_val = phoenix::construct<conf_file_t>(_1)] >>
      *(-comment >> eol) > eoi;

    BOOST_SPIRIT_DEBUG_NODE(heading);
    BOOST_SPIRIT_DEBUG_NODE(section);
    BOOST_SPIRIT_DEBUG_NODE(key);
    BOOST_SPIRIT_DEBUG_NODE(quoted_value);
    BOOST_SPIRIT_DEBUG_NODE(unquoted_value);
    BOOST_SPIRIT_DEBUG_NODE(key_val);
    BOOST_SPIRIT_DEBUG_NODE(conf_file);
  }
  qi::rule<Iterator> spaces;
  qi::rule<Iterator> comment_start;
  qi::rule<Iterator, char()> text_char;
  qi::rule<Iterator, std::string()> key;
  qi::rule<Iterator, std::string()> quoted_value;
  qi::rule<Iterator, std::string()> unquoted_value;
  qi::rule<Iterator> comment;
  qi::rule<Iterator, std::string()> value;
  qi::rule<Iterator, conf_item_t()> key_val;
  qi::rule<Iterator, std::string()> heading;
  qi::rule<Iterator, conf_section_t()> section;
  qi::rule<Iterator, conf_file_t()> conf_file;
};

int main(int argc, char* argv[])
{
  std::ifstream ifs(argv[1]);
  std::string buffer{std::istreambuf_iterator<char>(ifs),
		     std::istreambuf_iterator<char>()};
  auto iter = buffer.begin();
  auto end = buffer.end();
  IniGrammer<decltype(iter)> g;
  conf_file_t conf;
  if (qi::phrase_parse(iter, end, g, ascii::space, conf)) {
    std::cout << "success" << endl;
    std::cout << conf;
  } else {
    std::cerr << "fail" << endl;
  }
}
