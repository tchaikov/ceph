# -*- coding: utf-8 -*-
import errno
import inspect
import json
import logging
from collections.abc import Mapping
from typing import TYPE_CHECKING, Any, Callable, Dict, NamedTuple, \
    Optional, Type, Union

import yaml
from mgr_module import CLICommandBase, HandleCommandResult

from .formatter import AnnotatedDataTextOutputFormatter

logger = logging.getLogger(__name__)

NVMeoFCLICommand = CLICommandBase.make_registry_subtype("NVMeoFCLICommand")

# NVMeoFCLICommand is built by type() at import time, so it is a value as
# far as mypy is concerned and cannot be followed as a base class. Give
# the mixin the CLICommandBase it ends up in front of instead, and drop
# the registry subtype from the bases; the two are the same class to the
# type checker.
if TYPE_CHECKING:
    _MixinBase = CLICommandBase
    _RegistryBase = object
else:
    _MixinBase = object
    _RegistryBase = NVMeoFCLICommand


class NvmeofCLICommandMixin(_MixinBase):
    desc: str

    def __init__(self,
                 prefix: str,
                 model: Type[NamedTuple],
                 alias: Optional[str] = None,
                 perm: str = 'rw',
                 poll: bool = False,
                 success_message_template: Optional[str] = None,
                 success_message_map: Optional[Dict[str, Any]] = None,
                 success_message_fn: Optional[Callable[[Dict[str, Any]], str]] = None,
                 deprecated_params: Optional[Dict[str, str]] = None) -> None:
        super().__init__(prefix, perm, poll)
        self._output_formatter = AnnotatedDataTextOutputFormatter()
        self._model = model
        self._alias = alias
        self._alias_cmd: Optional['NvmeofCLICommandMixin'] = None

        self._success_message_template = success_message_template
        self._success_message_map = success_message_map or {}
        self._success_message_fn = success_message_fn
        self._func_defaults: Dict[str, Any] = {}
        self._deprecated_params: Dict[str, str] = deprecated_params or {}

    # the registered handlers return a model dict rather than the
    # (retval, stdout, stderr) tuple HandlerFuncType describes; call()
    # below is what turns one into a HandleCommandResult
    def __call__(self, func: Callable[..., Any]) -> Any:
        resp = super().__call__(func)
        self._func_defaults = self._compute_func_defaults()

        if self._alias:
            self._alias_cmd = type(self)(
                self._alias,
                model=self._model,
                perm=self.perm,
                poll=self.poll,
                success_message_template=self._success_message_template,
                success_message_map=self._success_message_map,
                success_message_fn=self._success_message_fn,
                deprecated_params=self._deprecated_params,
            )
            self._alias_cmd(func)
            self._alias_cmd._func_defaults = self._alias_cmd._compute_func_defaults()

        return resp

    def _compute_func_defaults(self) -> Dict[str, Any]:
        defaults: Dict[str, Any] = {}
        assert self.func
        sig = inspect.signature(self.func)

        for name, param in sig.parameters.items():
            if name in self.KNOWN_ARGS:
                continue
            if name not in self.arg_spec:
                continue
            if param.default is not inspect.Parameter.empty:
                defaults[name] = param.default

        return defaults

    def _stringify(self, value: Any) -> str:
        if isinstance(value, (bytes, bytearray)):
            return value.decode('utf-8', errors='replace')

        if isinstance(value, (list, tuple)):
            try:
                return ','.join(self._stringify(v) for v in value)
            except (TypeError, ValueError):
                return str(value)

        return str(value)

    def _args_map_from_argspec(self,
                               cmd_dict: Dict[str, Any],
                               inbuf: Optional[str] = None) -> Dict[str, Any]:
        kwargs, specials = self._collect_args_by_argspec(cmd_dict)
        kwargs = kwargs or {}
        specials = specials or set()

        if inbuf and 'inbuf' in specials:
            kwargs['inbuf'] = inbuf

        return {**self._func_defaults, **kwargs}

    def _apply_single_map_spec(
        self,
        spec: Union[str, dict, Callable],
        raw: Any,
        fields: Dict[str, Any],
    ) -> Any:
        """
        spec can be:
          - dict mapping exact raw values to literal/callable
          - callable(value, fields)
          - literal value (e.g. string)
        """
        if callable(spec):
            return spec(raw, fields)

        if isinstance(spec, dict):
            if raw in spec:
                val = spec[raw]
                return val(raw, fields) if callable(val) else val
            return raw
        return spec

    def _apply_success_message_map(self, fields: Dict[str, Any]) -> Dict[str, Any]:
        out = dict(fields)
        for field, spec in self._success_message_map.items():
            raw = out.get(field)
            try:
                out[field] = self._apply_single_map_spec(spec, raw, out)
            # pylint: disable=broad-except
            except Exception:
                logger.warning("Failed applying success_message_map for field %s",
                               field, exc_info=True)
        return out

    def get_success_msg(self, args_map: Dict[str, Any], response: Any) -> Optional[str]:
        merged_fields: Dict[str, Any]

        if isinstance(response, Mapping):
            merged_fields = {**args_map, **dict(response)}
        else:
            merged_fields = dict(args_map)

        if self._success_message_fn:
            try:
                return self._success_message_fn(merged_fields)
            # pylint: disable=broad-except
            except Exception:
                logger.warning("success_message_fn failed for %s", self.prefix, exc_info=True)
                return None

        return self._format_success_message_from_args(args_map, response)

    def _format_success_message_from_args(self,
                                          args_map: Dict[str, Any],
                                          response: Any) -> Optional[str]:
        if not self._success_message_template:
            return None

        resp_map: Dict[str, Any] = dict(response) if isinstance(response, Mapping) else {}

        try:
            fields_dict = {**args_map, **resp_map}
            fields_dict = self._apply_success_message_map(fields_dict)
            str_map = {k: self._stringify(v) for k, v in fields_dict.items()}
            return self._success_message_template.format(**str_map)
        # pylint: disable=broad-except
        except Exception:
            logger.warning("Success message template failed for %s", self.prefix, exc_info=True)
            return None

    def _check_required_params(self, cmd_dict: Dict[str, Any]) -> Optional[str]:
        for name in self.arg_spec:
            if name in self.KNOWN_ARGS or name in self.defaulted_args:
                continue
            if cmd_dict.get(name) is None:
                return f"missing required parameter: --{name}"
        return None

    def call(self,
             mgr: Any,
             cmd_dict: Dict[str, Any],
             inbuf: Optional[str] = None) -> HandleCommandResult:
        deprecated_warnings = ''
        try:
            missing = self._check_required_params(cmd_dict)
            if missing:
                return HandleCommandResult(-errno.EINVAL, '', missing)
            out_format = cmd_dict.get('format')
            args_map = self._args_map_from_argspec(cmd_dict, inbuf)

            if out_format == 'plain' or not out_format:
                for param, msg in self._deprecated_params.items():
                    if args_map.get(param) is not None:
                        deprecated_warnings += f"\nWarning: {msg}"

            # the registered handlers return the model dict the output
            # formatter expects, not the HandleCommandResult the base
            # class advertises
            ret: Any = super().call(mgr, cmd_dict, inbuf)
            if ret is None:
                ret = {}

            if out_format == 'plain' or not out_format:
                message: Optional[str] = None
                try:
                    message = self.get_success_msg(args_map, ret)
                # pylint: disable=broad-except
                except Exception:
                    logger.warning("Formatting of success message failed for %s",
                                   self.prefix, exc_info=True)

                # Pass args_map as template_context for variable substitution in empty messages
                # This ensures parameters like 'nqn' are available without polluting the data
                out = message if message else self._output_formatter.format_output(
                    ret, self._model, template_context=args_map
                )
                wrn_msg = ret.get('error_message', '') if isinstance(ret, dict) else ''
                if wrn_msg:
                    out += f"\nWarning: {wrn_msg}"
                out += deprecated_warnings

            elif out_format == 'json':
                out = json.dumps(ret, indent=4)
            elif out_format == 'yaml':
                out = yaml.dump(ret)
            else:
                return HandleCommandResult(-errno.EINVAL, '',
                                           f"format '{out_format}' is not implemented")

            return HandleCommandResult(0, out, '')

        # pylint: disable=broad-except
        except Exception as e:
            return HandleCommandResult(-errno.EINVAL, '', str(e) + deprecated_warnings)


class NvmeofCLICommand(NvmeofCLICommandMixin, _RegistryBase):
    """CLI command registered on the nvmeof module's registry."""
