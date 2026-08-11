# -*- coding: utf-8 -*-
import logging
from abc import ABC, abstractmethod
from enum import Enum
from typing import Annotated, Any, Dict, List, NamedTuple, Optional, Type, \
    Union, get_args, get_origin, get_type_hints

from prettytable import PrettyTable

from .model import CliEmptyMessage, CliFieldTransformer, CliFlags, CliHeader
from .utils import convert_from_bytes

logger = logging.getLogger(__name__)


class OutputFormatter(ABC):
    @abstractmethod
    def format_output(self, data, model, template_context: Optional[Dict] = None):
        """Format the given data for output."""
        raise NotImplementedError()


class AnnotatedDataTextOutputFormatter(OutputFormatter):
    def _snake_case_to_title(self, s):
        return s.replace('_', ' ').title()

    def _create_table(self, field_names):
        table = PrettyTable(border=True)
        titles = [self._snake_case_to_title(field) for field in field_names]
        table.field_names = titles
        table.align = 'l'
        table.padding_width = 0
        return table

    def _get_text_output(self, data):
        if isinstance(data, list):
            return self._get_list_text_output(data)
        return self._get_object_text_output(data)

    def _get_row(self, columns, data_obj):
        row = []
        for col in columns:
            col_val = data_obj.get(col)
            if col_val is None:
                col_val = ''
            row.append(str(col_val))
        return row

    def _get_list_text_output(self, data):
        columns = list(dict.fromkeys([key for obj in data for key in obj.keys()]))
        if not columns:
            return ''
        table = self._create_table(columns)
        for d in data:
            table.add_row(self._get_row(columns, d))
        return table.get_string()

    def _get_object_text_output(self, data):
        columns = [k for k in data.keys() if k not in ["status", "error_message"]]
        if not columns:
            return ''
        table = self._create_table(columns)
        table.add_row(self._get_row(columns, data))
        return table.get_string()

    def _is_list_of_complex_type(self, value):
        if not isinstance(value, list):
            return False

        if not value:
            return None

        primitives = (int, float, str, bool, bytes)

        return not isinstance(value[0], primitives)

    def _select_list_field(self, data: Dict) -> Optional[str]:
        for key, value in data.items():
            if self._is_list_of_complex_type(value):
                return key
        return None

    def is_namedtuple_type(self, obj):
        return isinstance(obj, type) and issubclass(obj, tuple) and hasattr(obj, '_fields')

    def get_enum_class(self, maybe_enum: Any) -> Optional[Type[Enum]]:
        if isinstance(maybe_enum, type) and issubclass(maybe_enum, Enum):
            return maybe_enum
        if issubclass(type(maybe_enum), Enum):
            return type(maybe_enum)
        return None

    # pylint: disable=too-many-branches, too-many-nested-blocks
    def process_dict(self, input_dict: dict,
                     nt_class: Type[NamedTuple],
                     is_top_level: bool,
                     template_context: Optional[Dict] = None) -> Union[Dict, str, List]:
        result: Dict = {}
        if not input_dict:
            return result
        hints = get_type_hints(nt_class, include_extras=True)

        for field, type_hint in hints.items():
            if field not in input_dict:
                continue

            value = input_dict[field]
            origin = get_origin(type_hint)

            actual_type = type_hint
            annotations = []
            output_name = field
            skip = False
            empty_message_template = None

            if origin is Annotated:
                actual_type, *annotations = get_args(type_hint)
                for annotation in annotations:
                    if annotation == CliFlags.DROP:
                        skip = True
                        break
                    if isinstance(annotation, CliHeader):
                        output_name = annotation.label
                    if isinstance(annotation, CliEmptyMessage):
                        empty_message_template = annotation.template

                for annotation in annotations:
                    if isinstance(annotation, CliFieldTransformer):
                        value = annotation.transform(value)
                    if is_top_level and annotation == CliFlags.EXCLUSIVE_LIST:
                        assert get_origin(actual_type) == list
                        assert len(get_args(actual_type)) == 1
                        if not value and empty_message_template:
                            format_dict = {**input_dict}
                            if template_context:
                                format_dict.update(template_context)
                            try:
                                return empty_message_template.format(**format_dict)
                            except KeyError as e:
                                logger.warning(
                                    "Missing template variable %s in empty message template: %s",
                                    e, empty_message_template
                                )
                                # Fall back to returning the template as-is if formatting fails
                                return empty_message_template
                        return [self.process_dict(item, get_args(actual_type)[0],
                                                  False, template_context) for item in value]
                    if is_top_level and annotation == CliFlags.EXCLUSIVE_RESULT:
                        return f"Failure: {input_dict.get('error_message')}" if bool(
                            input_dict[field]) else "Success"
                    if annotation == CliFlags.SIZE:
                        value = convert_from_bytes(int(input_dict[field]))
                    elif annotation == CliFlags.PROMOTE_INTERNAL_FIELDS:
                        object_to_promote = self.process_dict(
                            value, actual_type, False, template_context)
                        if isinstance(object_to_promote, dict):
                            for field_name, value in object_to_promote.items():
                                result[field_name] = value
                            skip = True

            if skip:
                continue

            # If it's a nested namedtuple and value is a dict, recurse
            if self.is_namedtuple_type(actual_type) and isinstance(value, dict):
                result[output_name] = self.process_dict(value, actual_type, False)
            # If it's an enum type or enum instance, take its name
            elif (enum_cls := self.get_enum_class(actual_type)):
                result[output_name] = enum_cls(value).name
            else:
                result[output_name] = value

        return result

    def _convert_to_text_output(self, data, model, template_context: Optional[Dict] = None):
        data = self.process_dict(data, model, True, template_context)
        if isinstance(data, str):
            return data
        return self._get_text_output(data)

    def format_output(self, data, model, template_context: Optional[Dict] = None):
        return self._convert_to_text_output(data, model, template_context)
