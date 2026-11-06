#include "excellon.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

_Static_assert(_Alignof(ExcHit) <= _Alignof(long double),
               "Arena alignment is insufficient for ExcHit");

typedef enum {
  EXC_STATE_BEFORE_HEADER,
  EXC_STATE_HEADER,
  EXC_STATE_BODY,
  EXC_STATE_DONE
} ExcParseState;

#define EXC_SELECTED_TOOL_NONE UINT32_MAX
#define EXC_SELECTED_TOOL_UNDEFINED (UINT32_MAX - 1u)
#define EXC_MAX_FILE_SIZE (64u * 1024u * 1024u)

static bool next_line(const char *text, size_t length, size_t *position,
                      const char **line, size_t *line_length);
static void trim_line(const char **line, size_t *line_length);

static bool count_input_lines(const char *text, size_t length,
                              size_t *out_count) {
  if (text == NULL || out_count == NULL) {
    return false;
  }

  size_t position = 0;
  size_t count = 0;

  const char *line;
  size_t line_length;

  while (next_line(text, length, &position, &line, &line_length)) {
    ++count;
  }

  *out_count = count;
  return true;
}

static bool next_csv_field(const char *text, size_t length, size_t *position,
                           const char **field, size_t *field_length) {
  if (text == NULL || position == NULL || field == NULL ||
      field_length == NULL) {
    return false;
  }

  if (*position > length) {
    return false;
  }

  size_t start = *position;
  size_t end = start;

  while (end < length && text[end] != ',') {
    ++end;
  }
  *field = text + start;
  *field_length = end - start;

  *position = end + 1;

  return true;
}

static ExcSeverity issue_severity(ExcIssueCode code) {
  switch (code) {
  case EXC_ISSUE_ROUNDED:
  case EXC_ISSUE_UNKNOWN_COMMAND:
  case EXC_ISSUE_MISSING_END:
    return EXC_SEV_WARNING;
  case EXC_ISSUE_MISSING_HEADER_END:
    return EXC_SEV_ERROR;
  case EXC_ISSUE_NONE:
    return EXC_SEV_INFO;

  default:
    return EXC_SEV_ERROR;
  }
}

const ExcTool *exc_find_tool(const ExcFile *file, uint32_t number) {
  if (file == NULL || file->tool_count > EXC_MAX_TOOLS) {
    return NULL;
  }
  for (size_t i = 0; i < file->tool_count; ++i) {
    const ExcTool *tool = &file->tools[i];

    if (tool->defined && tool->number == number) {
      return tool;
    }
  }

  return NULL;
}

static bool line_equals(const char *line, size_t line_length,
                        const char *word) {
  if (line == NULL || word == NULL) {
    return false;
  }
  size_t i = 0;

  while (i < line_length) {
    if (word[i] == '\0' || line[i] != word[i]) {
      return false;
    }
    ++i;
  }
  return word[i] == '\0';
}

static bool parse_u32(const char *text, size_t length, uint32_t *out) {
  if (text == NULL || out == NULL || length == 0) {
    return false;
  }

  uint32_t value = 0;

  for (size_t i = 0; i < length; ++i) {
    char c = text[i];

    if (c < '0' || c > '9') {
      return false;
    }
    uint32_t digit = (uint32_t)(c - '0');

    if (value > (UINT32_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }

  *out = value;
  return true;
}

static void add_issue(ExcFile *file, ExcIssueCode code, uint32_t line,
                      uint32_t tool) {
  if (file == NULL || code == EXC_ISSUE_NONE) {
    return;
  }

  ExcSeverity severity = issue_severity(code);

  if (severity == EXC_SEV_ERROR) {
    file->error_count++;
  } else if (severity == EXC_SEV_WARNING) {
    file->warning_count++;
  }

  if (file->issue_count >= EXC_MAX_ISSUES) {
    file->issues_dropped++;
    return;
  }

  ExcIssue *issue = &file->issues[file->issue_count];

  issue->code = code;
  issue->severity = severity;
  issue->line = line;
  issue->tool = tool;

  file->issue_count++;
}

const char *exc_status_name(ExcStatus status) {
  static const char *const names[] = {
      "OK",       "CONTENT_ERROR", "ARGUMENT_ERROR",
      "IO_ERROR", "MEMORY_ERROR",  "LIMIT_ERROR"};

  _Static_assert(sizeof(names) / sizeof(names[0]) == 6,
                 "ExcStatus name table is incomplete");

  if (status > EXC_OK || status < EXC_ERR_LIMIT) {
    return "UNKNOWN_STATUS";
  }

  return names[-status];
}

const char *exc_issue_text(ExcIssueCode code) {
  static const char *const names[] = {"No issue",
                                      "Missing Excellon header",
                                      "Missing units declaration",
                                      "Missing end command",
                                      "Unknown command",
                                      "Unsupported coordinate format",
                                      "Unsupported routed slot",
                                      "Invalid number",
                                      "Coordinate out of range",
                                      "Coordinate rounded",
                                      "Duplicate drill tool",
                                      "Undefined drill tool",
                                      "Hit before tool selection",
                                      "Invalid drill diameter",
                                      "Missing Excellon header end"};

  _Static_assert(sizeof(names) / sizeof(names[0]) ==
                     (size_t)(EXC_ISSUE_MISSING_HEADER_END + 1),
                 "ExcIssueCode text table is incomplete");

  if ((unsigned)code > (unsigned)EXC_ISSUE_MISSING_HEADER_END) {
    return "Unknown issue";
  }

  return names[code];
}

ExcIssueCode exc_decimal_to_nm(const char *text, size_t length, ExcUnits units,
                               G2Nm *out_nm) {
  if (text == NULL || out_nm == NULL || length == 0) {
    return EXC_ISSUE_BAD_NUMBER;
  }

  if (units != EXC_UNITS_METRIC && units != EXC_UNITS_INCH) {
    return EXC_ISSUE_MISSING_UNITS;
  }

  size_t i = 0;
  bool negative = false;

  if (text[i] == '-') {
    negative = true;
    i++;
  } else if (text[i] == '+') {
    i++;
  }

  if (i == length) {
    return EXC_ISSUE_BAD_NUMBER;
  }

  uint64_t mantissa = 0;
  size_t fractional_digits = 0;
  size_t significant_digits = 0;
  size_t pending_fractional_zeros = 0;

  bool seen_dot = false;
  bool saw_digit = false;

  // Cap behavior: a value with more than 11 significant digits
  // (like 8.123456789012) returns COORD_RANGE rather than being rounded. That's
  // fine for KiCad output, which prints at most about 4 integer and 6 fraction
  // digits.
  const size_t max_significant_digits = 11;

  for (; i < length; i++) {
    char c = text[i];

    if (c == '.') {
      if (seen_dot) {
        return EXC_ISSUE_BAD_NUMBER;
      }
      seen_dot = true;
      continue;
    }

    if (c < '0' || c > '9') {
      return EXC_ISSUE_BAD_NUMBER;
    }

    unsigned digit = (unsigned)(c - '0');
    saw_digit = true;

    if (seen_dot) {
      fractional_digits++;

      if (digit == 0) {
        pending_fractional_zeros++;
        continue;
      }
    }

    if (pending_fractional_zeros > 0) {
      if (mantissa != 0) {
        if (pending_fractional_zeros >
            max_significant_digits - significant_digits) {
          return EXC_ISSUE_COORD_RANGE;
        }

        for (size_t z = 0; z < pending_fractional_zeros; z++) {
          mantissa *= 10;
          significant_digits++;
        }
      }
      pending_fractional_zeros = 0;
    }

    if (digit != 0 || mantissa != 0) {
      if (significant_digits >= max_significant_digits) {
        return EXC_ISSUE_COORD_RANGE;
      }
      if (mantissa > (UINT64_MAX - digit) / 10) {
        return EXC_ISSUE_COORD_RANGE;
      }
      mantissa = mantissa * 10 + digit;
      significant_digits++;
    }
  }

  if (!saw_digit) {
    return EXC_ISSUE_BAD_NUMBER;
  }

  fractional_digits -= pending_fractional_zeros;

  uint64_t multiplier;
  if (units == EXC_UNITS_METRIC) {
    multiplier = 1000000ULL;
  } else {
    multiplier = 25400000ULL;
  }

  static const uint64_t powers10[] = {1ULL,
                                      10ULL,
                                      100ULL,
                                      1000ULL,
                                      10000ULL,
                                      100000ULL,
                                      1000000ULL,
                                      10000000ULL,
                                      100000000ULL,
                                      1000000000ULL,
                                      10000000000ULL,
                                      100000000000ULL,
                                      1000000000000ULL,
                                      10000000000000ULL,
                                      100000000000000ULL,
                                      1000000000000000ULL,
                                      10000000000000000ULL,
                                      100000000000000000ULL,
                                      1000000000000000000ULL};

  uint64_t numerator = mantissa * multiplier;
  uint64_t quotient;
  uint64_t remainder;

  if (fractional_digits > 18) {
    quotient = 0;
    remainder = numerator;
  } else {
    uint64_t divisor = powers10[fractional_digits];
    quotient = numerator / divisor;
    remainder = numerator % divisor;
  }

  bool rounded = (remainder != 0);

  if (fractional_digits <= 18) {
    uint64_t divisor = powers10[fractional_digits];
    if (remainder >= divisor - remainder) {
      quotient++;
    }
  }

  if (quotient > (uint64_t)INT64_MAX) {
    return EXC_ISSUE_COORD_RANGE;
  }

  int64_t signed_result = (int64_t)quotient;
  if (negative) {
    signed_result = -signed_result;
  }

  *out_nm = (G2Nm)signed_result;

  return rounded ? EXC_ISSUE_ROUNDED : EXC_ISSUE_NONE;
}

static bool next_line(const char *text, size_t length, size_t *position,
                      const char **line, size_t *line_length) {
  if (text == NULL || position == NULL || line == NULL || line_length == NULL) {
    return false;
  }

  if (*position > length) {
    return false;
  }

  if (*position == length) {
    return false;
  }

  size_t start = *position;
  size_t end = start;

  while (end < length && text[end] != '\n' && text[end] != '\r') {
    ++end;
  }
  *line = text + start;
  *line_length = end - start;

  if (end < length) {
    if (text[end] == '\r' && end + 1 < length && text[end + 1] == '\n') {
      end += 2;
    } else {
      end += 1;
    }
  }

  *position = end;
  return true;
}

static void trim_line(const char **line, size_t *line_length) {
  if (line == NULL || line_length == NULL || *line == NULL) {
    return;
  }

  while (*line_length > 0 && (**line == ' ' || **line == '\t')) {
    ++(*line);
    --(*line_length);
  }
  while (*line_length > 0 && ((*line)[*line_length - 1] == ' ' ||
                              (*line)[*line_length - 1] == '\t')) {
    --(*line_length);
  }
}

static ExcStatus parse_tool_definition(ExcFile *file, const char *line,
                                       size_t line_length, uint32_t line_number,
                                       ExcPlating current_plating,
                                       ExcDrillFunction current_function) {
  if (file == NULL || line == NULL || line_length == 0 || line[0] != 'T') {
    return EXC_ERR_ARGUMENT;
  }

  size_t c_pos = 1;
  while (c_pos < line_length && line[c_pos] != 'C') {
    ++c_pos;
  }

  if (c_pos == line_length || c_pos == 1 || c_pos + 1 == line_length) {
    add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
    return EXC_OK;
  }

  uint32_t number = 0;
  if (c_pos - 1 > 2 || !parse_u32(line + 1, c_pos - 1, &number) ||
      number == 0 || number >= EXC_MAX_TOOLS) {
    add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
    return EXC_OK;
  }

  if (exc_find_tool(file, number) != NULL) {
    add_issue(file, EXC_ISSUE_DUPLICATE_TOOL, line_number, number);
    return EXC_OK;
  }

  if (file->tool_count >= EXC_MAX_TOOLS) {
    return EXC_ERR_LIMIT;
  }

  // CAM tools may append feed/speed/etc. parameters after the diameter.
  size_t diameter_end = c_pos + 1;
  while (diameter_end < line_length) {
    char c = line[diameter_end];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      break;
    }
    ++diameter_end;
  }

  if (diameter_end == c_pos + 1) {
    add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, number);
    return EXC_OK;
  }

  G2Nm diameter = 0;
  ExcIssueCode result = exc_decimal_to_nm(
      line + c_pos + 1, diameter_end - c_pos - 1, file->units, &diameter);

  if (result == EXC_ISSUE_ROUNDED) {
    add_issue(file, EXC_ISSUE_ROUNDED, line_number, number);
  } else if (result != EXC_ISSUE_NONE) {
    add_issue(file, result, line_number, number);
    return EXC_OK;
  }

  if (diameter <= 0) {
    add_issue(file, EXC_ISSUE_BAD_DIAMETER, line_number, number);
    return EXC_OK;
  }

  ExcTool *tool = &file->tools[file->tool_count];
  tool->number = number;
  tool->diameter = diameter;
  tool->plating = current_plating;

  if (tool->plating == EXC_PLATING_UNKNOWN) {
    if (file->file_plating == EXC_PLATING_PLATED ||
        file->file_plating == EXC_PLATING_NONPLATED) {
      tool->plating = file->file_plating;
    }
  }

  tool->function = current_function;
  tool->defined = true;
  tool->line = line_number;
  tool->hit_count = 0;
  tool->slot_count = 0;

  ++file->tool_count;
  return EXC_OK;
}

static ExcStatus parse_ta_attribute(ExcFile *file, const char *line,
                                    size_t line_length, uint32_t line_number,
                                    ExcPlating *current_plating,
                                    ExcDrillFunction *current_function) {

  if (file == NULL || line == NULL || current_plating == NULL ||
      current_function == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  static const char prefix[] = "; #@! TA.AperFunction,";
  const size_t prefix_length = sizeof(prefix) - 1;

  if (line_length < prefix_length || memcmp(line, prefix, prefix_length) != 0) {
    return EXC_ERR_ARGUMENT;
  }

  *current_plating = EXC_PLATING_UNKNOWN;
  *current_function = EXC_DRILL_UNKNOWN;

  bool unknown_token = false;
  bool plating_conflict = false;
  bool function_conflict = false;

  size_t pos = prefix_length;

  while (pos < line_length) {
    size_t start = pos;

    while (pos < line_length && line[pos] != ',') {
      ++pos;
    }

    size_t token_length = pos - start;
    const char *token = line + start;

    if (token_length == 0) {
      unknown_token = true;
    }

    ExcPlating token_plating = EXC_PLATING_UNKNOWN;

    if (line_equals(token, token_length, "Plated")) {
      token_plating = EXC_PLATING_PLATED;
    } else if (line_equals(token, token_length, "NonPlated")) {
      token_plating = EXC_PLATING_NONPLATED;
    } else if (line_equals(token, token_length, "PTH")) {
      token_plating = EXC_PLATING_PLATED;
    } else if (line_equals(token, token_length, "NPTH")) {
      token_plating = EXC_PLATING_NONPLATED;
    }

    if (token_plating != EXC_PLATING_UNKNOWN) {
      if (*current_plating == EXC_PLATING_UNKNOWN) {
        *current_plating = token_plating;
      } else if (*current_plating != token_plating) {
        plating_conflict = true;
      }
    }

    ExcDrillFunction token_function = EXC_DRILL_UNKNOWN;

    if (line_equals(token, token_length, "ViaDrill")) {
      token_function = EXC_DRILL_VIA;
    } else if (line_equals(token, token_length, "ComponentDrill")) {
      token_function = EXC_DRILL_COMPONENT;
    } else if (line_equals(token, token_length, "MechanicalDrill")) {
      token_function = EXC_DRILL_MECHANICAL;
    } else if (line_equals(token, token_length, "BackDrill") ||
               line_equals(token, token_length, "OtherDrill") ||
               line_equals(token, token_length, "CastellatedDrill")) {
      token_function = EXC_DRILL_OTHER;
    }

    if (token_function != EXC_DRILL_UNKNOWN) {
      if (*current_function == EXC_DRILL_UNKNOWN) {
        *current_function = token_function;
      } else if (*current_function != token_function) {
        function_conflict = true;
      }
    }

    bool known_qualifier = line_equals(token, token_length, "Buried") ||
                           line_equals(token, token_length, "PressFit") ||
                           line_equals(token, token_length, "PTH") ||
                           line_equals(token, token_length, "NPTH") ||
                           line_equals(token, token_length, "Plated") ||
                           line_equals(token, token_length, "NonPlated");

    if (token_function == EXC_DRILL_UNKNOWN && !known_qualifier) {
      unknown_token = true;
    }

    if (pos < line_length) {
      ++pos;
    }
  }

  if (line_length > prefix_length && line[line_length - 1] == ',') {
    unknown_token = true;
  }

  if (plating_conflict) {
    *current_plating = EXC_PLATING_UNKNOWN;
    unknown_token = true;
  }

  if (function_conflict) {
    *current_function = EXC_DRILL_UNKNOWN;
    unknown_token = true;
  }

  if (unknown_token) {
    add_issue(file, EXC_ISSUE_UNKNOWN_COMMAND, line_number, 0);
  }

  return EXC_OK;
}

static ExcStatus parse_tf_filefunction(ExcFile *file, const char *line,
                                       size_t line_length,
                                       uint32_t line_number) {
  if (file == NULL || line == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  static const char prefix[] = "; #@! TF.FileFunction,";
  const size_t prefix_length = sizeof(prefix) - 1;
  if (line_length < prefix_length || memcmp(line, prefix, prefix_length) != 0) {
    return EXC_ERR_ARGUMENT;
  }

  size_t position = prefix_length;
  const char *field = NULL;
  size_t field_length = 0;

  if (!next_csv_field(line, line_length, &position, &field, &field_length)) {
    add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
    return EXC_OK;
  }

  if (line_equals(field, field_length, "Plated")) {
    file->file_plating = EXC_PLATING_PLATED;
  } else if (line_equals(field, field_length, "NonPlated")) {
    file->file_plating = EXC_PLATING_NONPLATED;
  } else if (line_equals(field, field_length, "MixedPlating")) {
    file->file_plating = EXC_PLATING_MIXED;
  } else {
    add_issue(file, EXC_ISSUE_UNKNOWN_COMMAND, line_number, 0);
    return EXC_OK;
  }

  // Layer numbers are optional. A malformed supplied layer is diagnosed,
  // but does not undo the already-recognized file plating.
  file->layer_from = 0;
  file->layer_to = 0;

  if (next_csv_field(line, line_length, &position, &field, &field_length)) {
    uint32_t layer_from = 0;
    if (parse_u32(field, field_length, &layer_from) && layer_from > 0 &&
        layer_from <= INT_MAX) {
      file->layer_from = (int)layer_from;
    } else {
      add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
    }
  }

  if (next_csv_field(line, line_length, &position, &field, &field_length)) {
    uint32_t layer_to = 0;
    if (parse_u32(field, field_length, &layer_to) && layer_to > 0 &&
        layer_to <= INT_MAX) {
      file->layer_to = (int)layer_to;
    } else {
      add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
    }
  }

  return EXC_OK;
}

static bool is_decimal_format_mask(const char *text, size_t length) {
  if (text == NULL || length < 3) {
    return false;
  }

  bool saw_dot = false;
  bool saw_digit_before = false;
  bool saw_digit_after = false;

  for (size_t i = 0; i < length; ++i) {
    if (text[i] == '.') {
      if (saw_dot) {
        return false;
      }
      saw_dot = true;
    } else if (text[i] == '0') {
      if (saw_dot) {
        saw_digit_after = true;
      } else {
        saw_digit_before = true;
      }
    } else {
      return false;
    }
  }

  return saw_dot && saw_digit_before && saw_digit_after;
}

static ExcStatus parse_units_line(ExcFile *file, const char *line,
                                  size_t line_length, uint32_t line_number) {
  if (file == NULL || line == NULL || line_length == 0) {
    return EXC_ERR_ARGUMENT;
  }

  size_t comma = 0;
  while (comma < line_length && line[comma] != ',') {
    ++comma;
  }

  ExcUnits units;
  if (line_equals(line, comma, "METRIC")) {
    units = EXC_UNITS_METRIC;
  } else if (line_equals(line, comma, "INCH")) {
    units = EXC_UNITS_INCH;
  } else {
    add_issue(file, EXC_ISSUE_UNKNOWN_COMMAND, line_number, 0);
    return EXC_OK;
  }

  bool units_conflict =
      file->units != EXC_UNITS_UNKNOWN && file->units != units;
  if (units_conflict) {
    add_issue(file, EXC_ISSUE_UNSUPPORTED_FORMAT, line_number, 0);
  } else {
    // Commit units before suffix validation so a bad suffix cannot leave
    // every subsequent numeric field in the UNKNOWN-units state.
    file->units = units;
  }

  ExcZeroSuppress zeros = EXC_ZEROS_DECIMAL;
  bool suffix_valid = true;

  if (comma < line_length) {
    size_t position = comma + 1;
    const char *suffix = NULL;
    size_t suffix_length = 0;

    if (!next_csv_field(line, line_length, &position, &suffix,
                        &suffix_length)) {
      suffix_valid = false;
    } else if (line_equals(suffix, suffix_length, "LZ")) {
      zeros = EXC_ZEROS_LEADING_OMITTED;

      // KiCad may append a decimal format mask after the zero mode.
      if (position <= line_length) {
        const char *mask = NULL;
        size_t mask_length = 0;
        if (next_csv_field(line, line_length, &position, &mask, &mask_length)) {
          suffix_valid = is_decimal_format_mask(mask, mask_length);
        }
      }
    } else if (line_equals(suffix, suffix_length, "TZ")) {
      zeros = EXC_ZEROS_TRAILING_OMITTED;

      if (position <= line_length) {
        const char *mask = NULL;
        size_t mask_length = 0;
        if (next_csv_field(line, line_length, &position, &mask, &mask_length)) {
          suffix_valid = is_decimal_format_mask(mask, mask_length);
        }
      }
    } else {
      // A bare decimal format mask (e.g. 000.000) still denotes an
      // explicit-decimal coordinate format.
      suffix_valid = is_decimal_format_mask(suffix, suffix_length);
    }

    // More than one optional suffix/mask is outside this reader's subset.
    if (suffix_valid && position <= line_length) {
      suffix_valid = false;
    }
  }

  if (!suffix_valid) {
    add_issue(file, EXC_ISSUE_UNSUPPORTED_FORMAT, line_number, 0);
  } else if (!units_conflict) {
    file->zeros = zeros;
  }

  return EXC_OK;
}

static ExcStatus parse_fmat_line(ExcFile *file, const char *line,
                                 size_t line_length, uint32_t line_number) {
  if (file == NULL || line == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  static const char prefix[] = "FMAT,";
  const size_t prefix_length = sizeof(prefix) - 1;

  if (line_length <= prefix_length ||
      memcmp(line, prefix, prefix_length) != 0) {
    add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
    return EXC_OK;
  }

  uint32_t format = 0;

  if (!parse_u32(line + prefix_length, line_length - prefix_length, &format)) {
    add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
    return EXC_OK;
  }

  file->fmat = format;

  if (format != 2) {
    add_issue(file, EXC_ISSUE_UNSUPPORTED_FORMAT, line_number, 0);
  }

  return EXC_OK;
}

static bool line_starts_with(const char *line, size_t line_length,
                             const char *prefix) {
  if (line == NULL || prefix == NULL) {
    return false;
  }

  size_t prefix_length = strlen(prefix);

  return line_length >= prefix_length &&
         memcmp(line, prefix, prefix_length) == 0;
}

static bool line_contains(const char *line, size_t line_length,
                          const char *word) {
  if (line == NULL || word == NULL) {
    return false;
  }

  size_t word_length = strlen(word);

  if (word_length == 0 || word_length > line_length) {
    return false;
  }

  for (size_t i = 0; i <= line_length - word_length; ++i) {
    if (memcmp(line + i, word, word_length) == 0) {
      return true;
    }
  }

  return false;
}

static ExcStatus parse_header_line(ExcFile *file, const char *line,
                                   size_t line_length, uint32_t line_number,
                                   ExcParseState *state,
                                   ExcPlating *current_plating,
                                   ExcDrillFunction *current_function) {

  if (file == NULL || line == NULL || state == NULL ||
      current_plating == NULL || current_function == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  if (line_length == 0) {
    return EXC_OK;
  }

  if (line_equals(line, line_length, "%")) {
    file->saw_header_end = true;
    *state = EXC_STATE_BODY;
    return EXC_OK;
  }

  if (line_equals(line, line_length, "M95")) {
    file->saw_header_end = true;
    *state = EXC_STATE_BODY;
    return EXC_OK;
  }

  if (line_equals(line, line_length, "M48")) {
    return EXC_OK;
  }

  if (line[0] == ';') {
    if (line_starts_with(line, line_length, "; #@! TA.AperFunction,")) {
      return parse_ta_attribute(file, line, line_length, line_number,
                                current_plating, current_function);
    }

    if (line_equals(line, line_length, "; #@! TD")) {
      *current_plating = EXC_PLATING_UNKNOWN;
      *current_function = EXC_DRILL_UNKNOWN;
      return EXC_OK;
    }

    if (line_starts_with(line, line_length, "; #@! TF.FileFunction,")) {
      return parse_tf_filefunction(file, line, line_length, line_number);
    }

    return EXC_OK;
  }

  if (line_equals(line, line_length, "METRIC") ||
      line_starts_with(line, line_length, "METRIC,")) {
    return parse_units_line(file, line, line_length, line_number);
  }

  if (line_equals(line, line_length, "INCH") ||
      line_starts_with(line, line_length, "INCH,")) {
    return parse_units_line(file, line, line_length, line_number);
  }

  if (line_starts_with(line, line_length, "FMAT,")) {
    return parse_fmat_line(file, line, line_length, line_number);
  }

  if (line_equals(line, line_length, "G90")) {
    file->absolute = true;
    return EXC_OK;
  }

  if (line_equals(line, line_length, "G91")) {
    file->absolute = false;
    add_issue(file, EXC_ISSUE_UNSUPPORTED_FORMAT, line_number, 0);
    return EXC_OK;
  }

  if (line_equals(line, line_length, "G05")) {
    return EXC_OK;
  }

  if (line[0] == 'T') {
    return parse_tool_definition(file, line, line_length, line_number,
                                 *current_plating, *current_function);
  }

  add_issue(file, EXC_ISSUE_UNKNOWN_COMMAND, line_number, 0);
  return EXC_OK;
}

static bool parse_coordinate(ExcFile *file, const char *text, size_t length,
                             uint32_t line_number, uint32_t tool_number,
                             G2Nm *out) {
  if (file == NULL || text == NULL || out == NULL) {
    return false;
  }

  if (file->zeros != EXC_ZEROS_DECIMAL && memchr(text, '.', length) == NULL) {
    add_issue(file, EXC_ISSUE_UNSUPPORTED_FORMAT, line_number, tool_number);
    return false;
  }

  G2Nm value = 0;

  ExcIssueCode result = exc_decimal_to_nm(text, length, file->units, &value);

  if (result == EXC_ISSUE_ROUNDED) {
    add_issue(file, EXC_ISSUE_ROUNDED, line_number, tool_number);
  } else if (result != EXC_ISSUE_NONE) {
    add_issue(file, result, line_number, tool_number);
    return false;
  }

  if (value < -G2_COORD_MAX_NM || value > G2_COORD_MAX_NM) {
    add_issue(file, EXC_ISSUE_COORD_RANGE, line_number, tool_number);
    return false;
  }

  *out = value;
  return true;
}

static ExcStatus parse_body_line(ExcFile *file, const char *line,
                                 size_t line_length, uint32_t line_number,
                                 uint32_t *selected_tool,
                                 G2Point *modal_position, bool *have_x,
                                 bool *have_y, bool *routing_mode,
                                 bool *routing_issue_reported,
                                 size_t hit_capacity) {
  if (file == NULL || line == NULL || selected_tool == NULL ||
      modal_position == NULL || have_x == NULL || have_y == NULL ||
      routing_mode == NULL || routing_issue_reported == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  if (line_length == 0 || line[0] == ';') {
    return EXC_OK;
  }

  // A tool definition may legally appear in the body as TnCdiameter.
  if (line[0] == 'T') {
    size_t c_pos = 1;
    while (c_pos < line_length && line[c_pos] != 'C') {
      ++c_pos;
    }
    if (c_pos < line_length) {
      return parse_tool_definition(file, line, line_length, line_number,
                                   EXC_PLATING_UNKNOWN, EXC_DRILL_UNKNOWN);
    }

    // A new T-selection command invalidates the previous selection until
    // this command has been parsed and resolved successfully.
    *selected_tool = EXC_SELECTED_TOOL_UNDEFINED;

    uint32_t number = 0;
    if (!parse_u32(line + 1, line_length - 1, &number)) {
      add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, 0);
      return EXC_OK;
    }

    if (number >= EXC_MAX_TOOLS) {
      add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, number);
      return EXC_OK;
    }

    if (number == 0) {
      *selected_tool = EXC_SELECTED_TOOL_NONE;
      return EXC_OK;
    }

    const ExcTool *tool = exc_find_tool(file, number);
    if (tool == NULL) {
      add_issue(file, EXC_ISSUE_UNDEFINED_TOOL, line_number, number);
      *selected_tool = EXC_SELECTED_TOOL_UNDEFINED;
      return EXC_OK;
    }

    *selected_tool = (uint32_t)(tool - file->tools);
    return EXC_OK;
  }

  uint32_t tool_number = 0;
  if (*selected_tool < file->tool_count) {
    tool_number = file->tools[*selected_tool].number;
  }

  if (line_equals(line, line_length, "M15")) {
    if (!*routing_mode) {
      *routing_mode = true;
      *routing_issue_reported = false;
    }
    return EXC_OK;
  }

  if (line_equals(line, line_length, "M16")) {
    *routing_mode = false;
    *routing_issue_reported = false;
    return EXC_OK;
  }

  if (line_equals(line, line_length, "G05")) {
    *routing_mode = false;
    *routing_issue_reported = false;
    return EXC_OK;
  }

  if (line_equals(line, line_length, "M00")) {
    return EXC_OK;
  }

  if (line_equals(line, line_length, "G90")) {
    file->absolute = true;
    return EXC_OK;
  }

  if (line_equals(line, line_length, "G91")) {
    file->absolute = false;
    add_issue(file, EXC_ISSUE_UNSUPPORTED_FORMAT, line_number, tool_number);
    return EXC_OK;
  }

  // G85 is a self-contained slot command. G00/G01/G02/G03 begin or
  // continue routed operations; report the whole routed slot only once.
  if (line_contains(line, line_length, "G85")) {
    add_issue(file, EXC_ISSUE_UNSUPPORTED_SLOT, line_number, tool_number);
    return EXC_OK;
  }

  bool route_command = line_starts_with(line, line_length, "G00") ||
                       line_starts_with(line, line_length, "G01") ||
                       line_starts_with(line, line_length, "G02") ||
                       line_starts_with(line, line_length, "G03");

  if (route_command) {
    if (!*routing_mode) {
      *routing_mode = true;
      *routing_issue_reported = false;
    }
    if (!*routing_issue_reported) {
      add_issue(file, EXC_ISSUE_UNSUPPORTED_SLOT, line_number, tool_number);
      *routing_issue_reported = true;
    }
    return EXC_OK;
  }

  if (*routing_mode) {
    if (line_contains(line, line_length, "X") ||
        line_contains(line, line_length, "Y")) {
      if (!*routing_issue_reported) {
        add_issue(file, EXC_ISSUE_UNSUPPORTED_SLOT, line_number, tool_number);
        *routing_issue_reported = true;
      }
    }
    return EXC_OK;
  }

  // After selecting an undefined tool, ignore its coordinate records until
  // another T command changes the selection. Avoid cascading diagnostics.
  if (*selected_tool == EXC_SELECTED_TOOL_UNDEFINED) {
    if (line[0] == 'X' || line[0] == 'Y') {
      return EXC_OK;
    }
  }

  /* EXC_SELECTED_TOOL_UNDEFINED is not an index: its coordinate lines returned above, and any other line is
   * reported below without touching a tool. */
  if (*selected_tool != EXC_SELECTED_TOOL_NONE &&
      *selected_tool != EXC_SELECTED_TOOL_UNDEFINED &&
      *selected_tool >= file->tool_count) {
    return EXC_ERR_ARGUMENT;
  }

  if (!file->absolute) {
    add_issue(file, EXC_ISSUE_UNSUPPORTED_FORMAT, line_number, tool_number);
    return EXC_OK;
  }

  G2Point next_position = *modal_position;
  bool next_have_x = *have_x;
  bool next_have_y = *have_y;
  bool saw_x = false;
  bool saw_y = false;

  size_t pos = 0;
  while (pos < line_length) {
    char axis = line[pos++];
    if (axis != 'X' && axis != 'Y') {
      add_issue(file, EXC_ISSUE_UNKNOWN_COMMAND, line_number, tool_number);
      return EXC_OK;
    }

    size_t start = pos;
    while (pos < line_length && line[pos] != 'X' && line[pos] != 'Y') {
      ++pos;
    }

    if (start == pos) {
      add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, tool_number);
      return EXC_OK;
    }

    G2Nm coordinate = 0;
    if (axis == 'X') {
      if (saw_x) {
        add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, tool_number);
        return EXC_OK;
      }
      if (!parse_coordinate(file, line + start, pos - start, line_number,
                            tool_number, &coordinate)) {
        return EXC_OK;
      }
      next_position.x = coordinate;
      next_have_x = true;
      saw_x = true;
    } else {
      if (saw_y) {
        add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, tool_number);
        return EXC_OK;
      }
      if (!parse_coordinate(file, line + start, pos - start, line_number,
                            tool_number, &coordinate)) {
        return EXC_OK;
      }
      next_position.y = coordinate;
      next_have_y = true;
      saw_y = true;
    }
  }

  if (!saw_x && !saw_y) {
    add_issue(file, EXC_ISSUE_UNKNOWN_COMMAND, line_number, tool_number);
    return EXC_OK;
  }

  if (!next_have_x || !next_have_y) {
    add_issue(file, EXC_ISSUE_BAD_NUMBER, line_number, tool_number);
    return EXC_OK;
  }

  if (*selected_tool == EXC_SELECTED_TOOL_NONE) {
    add_issue(file, EXC_ISSUE_HIT_BEFORE_TOOL, line_number, 0);
    return EXC_OK;
  }

  if (file->hits == NULL || file->hit_count >= hit_capacity) {
    return EXC_ERR_LIMIT;
  }

  *modal_position = next_position;
  *have_x = next_have_x;
  *have_y = next_have_y;

  ExcHit *hit = &file->hits[file->hit_count];
  hit->pos = next_position;
  hit->tool = *selected_tool;
  hit->line = line_number;

  ++file->hit_count;
  ++file->tools[*selected_tool].hit_count;

  return EXC_OK;
}

ExcStatus exc_parse(Arena *arena, const char *text, size_t length,
                    const char *source_name, ExcFile *out) {

  if (out == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  memset(out, 0, sizeof(*out));
  out->absolute = true;

  if (arena == NULL || text == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  out->source_name = source_name;

  // Strip an optional UTF-8 BOM before line counting and parsing.
  if (length >= 3 && (unsigned char)text[0] == 0xEF &&
      (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
    text += 3;
    length -= 3;
  }

  size_t hit_capacity = 0;

  if (!count_input_lines(text, length, &hit_capacity)) {
    return EXC_ERR_ARGUMENT;
  }

  if (hit_capacity > UINT32_MAX) {
    return EXC_ERR_LIMIT;
  }

  if (hit_capacity > 0) {
    out->hits = arena_alloc_array(arena, sizeof(ExcHit), hit_capacity);

    if (out->hits == NULL) {
      return EXC_ERR_MEMORY;
    }
  }

  ExcParseState state = EXC_STATE_BEFORE_HEADER;

  ExcPlating current_plating = EXC_PLATING_UNKNOWN;
  ExcDrillFunction current_function = EXC_DRILL_UNKNOWN;

  uint32_t selected_tool = UINT32_MAX;

  G2Point modal_position = {0, 0};

  bool have_x = false;
  bool have_y = false;
  bool routing_mode = false;
  bool routing_issue_reported = false;

  size_t position = 0;
  uint32_t line_number = 0;

  const char *line;
  size_t line_length;

  while (next_line(text, length, &position, &line, &line_length)) {
    ++line_number;

    trim_line(&line, &line_length);

    if (line_length == 0) {
      continue;
    }

    if (state == EXC_STATE_DONE) {
      if (line[0] != ';') {
        add_issue(out, EXC_ISSUE_UNKNOWN_COMMAND, line_number, 0);
      }
      continue;
    }

    if (state == EXC_STATE_BEFORE_HEADER) {
      if (line[0] == ';') {
        continue;
      }

      if (line_equals(line, line_length, "M48")) {
        state = EXC_STATE_HEADER;
        continue;
      }

      add_issue(out, EXC_ISSUE_MISSING_HEADER, line_number, 0);
      state = EXC_STATE_HEADER;
    }

    if (state == EXC_STATE_HEADER) {
      ExcStatus status =
          parse_header_line(out, line, line_length, line_number, &state,
                            &current_plating, &current_function);

      if (status != EXC_OK) {
        return status;
      }

      continue;
    }

    if (state == EXC_STATE_BODY) {
      if (line_equals(line, line_length, "M30")) {
        out->saw_end = true;
        state = EXC_STATE_DONE;
        continue;
      }

      ExcStatus status =
          parse_body_line(out, line, line_length, line_number, &selected_tool,
                          &modal_position, &have_x, &have_y, &routing_mode,
                          &routing_issue_reported, hit_capacity);

      if (status != EXC_OK) {
        return status;
      }
    }
  }

  if (state == EXC_STATE_BEFORE_HEADER) {
    add_issue(out, EXC_ISSUE_MISSING_HEADER, 0, 0);
  }

  if (state == EXC_STATE_HEADER) {
    add_issue(out, EXC_ISSUE_MISSING_HEADER_END, 0, 0);
  }

  if (out->units == EXC_UNITS_UNKNOWN) {
    add_issue(out, EXC_ISSUE_MISSING_UNITS, 0, 0);
  }

  if (!out->saw_end) {
    add_issue(out, EXC_ISSUE_MISSING_END, 0, 0);
  }

  return out->error_count > 0 ? EXC_ERR_CONTENT : EXC_OK;
}

ExcStatus exc_parse_path(Arena *arena, const char *path, ExcFile *out) {

  if (out == NULL) {
    return EXC_ERR_ARGUMENT;
  }

  if (arena == NULL || path == NULL || path[0] == '\0') {
    return EXC_ERR_ARGUMENT;
  }

  FILE *stream = fopen(path, "rb");

  if (stream == NULL) {
    return EXC_ERR_IO;
  }

  if (fseek(stream, 0, SEEK_END) != 0) {
    fclose(stream);
    return EXC_ERR_IO;
  }

  long file_end = ftell(stream);

  if (file_end < 0) {
    fclose(stream);
    return EXC_ERR_IO;
  }

  if (fseek(stream, 0, SEEK_SET) != 0) {
    fclose(stream);
    return EXC_ERR_IO;
  }

  uintmax_t wide_size = (uintmax_t)file_end;

  if (wide_size > EXC_MAX_FILE_SIZE || wide_size > SIZE_MAX - 1) {
    fclose(stream);
    return EXC_ERR_LIMIT;
  }

  size_t file_size = (size_t)wide_size;

  char *buffer = arena_alloc(arena, file_size + 1);

  if (buffer == NULL) {
    fclose(stream);
    return EXC_ERR_MEMORY;
  }

  size_t bytes_read = fread(buffer, 1, file_size, stream);

  bool read_failed = bytes_read != file_size || ferror(stream);

  if (fclose(stream) != 0) {
    read_failed = true;
  }

  if (read_failed) {
    return EXC_ERR_IO;
  }

  buffer[file_size] = '\0';

  return exc_parse(arena, buffer, file_size, path, out);
}
